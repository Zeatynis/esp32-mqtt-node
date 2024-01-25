#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_err.h"
#include "esp_vfs_dev.h"
#include "driver/uart.h"
#include "esp_chip_info.h"
#include "network_common.h"

#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_tls.h"
#include "esp_ota_ops.h"
#include "esp_sntp.h"
#include <sys/param.h>

#include "driver/uart.h"
#include "sds011.h"

static const char *TAG = "ESP Sensor station";

#define SDS011_UART_PORT UART_NUM_2
#define SDS011_RX_GPIO 5
#define SDS011_TX_GPIO 4

/** Time in seconds to let the SDS011 run before taking the measurment. */
#define SDS011_ON_DURATION 15

/** Interval to read and print the sensor values in seconds. Must be bigger than
 * SDS011_ON_DURATION. */
#define PRINT_INTERVAL 60

extern const uint8_t mqtt_broker_cert_pem_start[]   asm("_binary_mqtt_broker_cert_pem_start");
extern const uint8_t mqtt_broker_cert_pem_end[]   asm("_binary_mqtt_broker_cert_pem_end");

static const struct sds011_tx_packet sds011_tx_sleep_packet = {
    .head = SDS011_PACKET_HEAD,
    .command = SDS011_CMD_TX,
    .sub_command = SDS011_TX_CMD_SLEEP_MODE,
    .payload_sleep_mode = {.method = SDS011_METHOD_SET,
                           .mode = SDS011_SLEEP_MODE_ENABLED},
    .device_id = SDS011_DEVICE_ID_ALL,
    .tail = SDS011_PACKET_TAIL};

static const struct sds011_tx_packet sds011_tx_wakeup_packet = {
    .head = SDS011_PACKET_HEAD,
    .command = SDS011_CMD_TX,
    .sub_command = SDS011_TX_CMD_SLEEP_MODE,
    .payload_sleep_mode = {.method = SDS011_METHOD_SET,
                           .mode = SDS011_SLEEP_MODE_DISABLED},
    .device_id = SDS011_DEVICE_ID_ALL,
    .tail = SDS011_PACKET_TAIL};

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_BEFORE_CONNECT:
        ESP_LOGI(TAG, "MQTT_EVENT_TRY_TO_CONNECT");
        break;
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        msg_id = esp_mqtt_client_subscribe(client, "topic/qos1", 1);
        ESP_LOGI(TAG, "Sent subscribe on 'topic/qos1', msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "Subscribed on 'topic/qos1'");
        msg_id = esp_mqtt_client_publish(client, "topic/qos0", "Cedalo is awesome", 0, 0, 0);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG,"Received on %.*s : %.*s",event->topic_len, event->topic, event->data_len,event->data);
        if (strncmp(event->data, "time", event->data_len) == 0) {
            time_t now = 0;
            struct tm timeinfo = { 0 };
            char strftime_buf[64];
            time(&now);
            localtime_r(&now, &timeinfo);
            strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
            ESP_LOGI(TAG, "ESP32-mqtt-node: My local time is: %s",strftime_buf);
            msg_id = esp_mqtt_client_publish(client, "topic/qos0", strftime_buf, 0, 0, 0);
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        break;
    default:
        ESP_LOGI(TAG, "Other not handled event id:%d", event->event_id);
        break;
    }
}

esp_err_t mqtt_start(void){
    // Create the mqtt client configuration
    esp_mqtt_client_config_t mqtt_cfg = {
            .credentials.client_id = "",
            .session.protocol_ver = MQTT_PROTOCOL_V_3_1_1 // MQTT_PROTOCOL_V_5
    };
    // Set some properties which are available with version 5.0
    // See all: https://github.com/espressif/esp-mqtt/blob/master/include/mqtt5_client.h
    // Example: https://github.com/espressif/esp-idf/tree/master/examples/protocols/mqtt5
    esp_mqtt5_connection_property_config_t connect_property = {
        .session_expiry_interval = 10,
        .request_resp_info = true,
        .request_problem_info = true,
        .message_expiry_interval = 10,
        .response_topic = "topic/response",
    };
    bool mqtt_v5 = false;
  
    // Get broker address from STDIN
    char buf[CONFIG_USER_INPUT_LENGTH] = {0};
    printf("Please enter url of mqtt broker:\n");
    int count = 0;
    while (count < CONFIG_USER_INPUT_LENGTH) {
        int c = fgetc(stdin);
        if (c == '\n' && count > 0) {
            buf[count] = '\0';
            break;
        } else if (c > 31 && c < 127) {
            buf[count] = c;
            ++count;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    mqtt_cfg.broker.address.uri = (char*)calloc(count+1,sizeof(char*));
    strncpy((char*)mqtt_cfg.broker.address.uri, buf, count+1);
    
    // Check if we need to enable certificate verififcation
    // "wss://" "mqtts://" --> enable
    char *scheme = strtok(buf,":");
    printf("Scheme: %S\n",scheme);
    if (strcmp(scheme,"mqtts")==0 || strcmp(scheme,"wss")==0){
        // use certificate for veritfication from file main/mqtt_broker_cert.pem
        mqtt_cfg.broker.verification.certificate = (const char *)mqtt_broker_cert_pem_start;
        ESP_LOGI(TAG,"TLS verification enabled");
    }

    // Get protocol version
    printf("Enable mqtt protocol version '5.0'? Type 'y' for yes, 'n' otherwise: \n");
    count = 0;
    while (count < 2) {
        int c = fgetc(stdin);
        if (c == '\n' && count > 0) {
            buf[count] = '\0';
            break;
        } else if (c > 31 && c < 127) {
            buf[count] = c;
            ++count;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    if (strcmp(buf,"y")==0){
        mqtt_cfg.session.protocol_ver = MQTT_PROTOCOL_V_5;
        mqtt_v5 = true;
    }
    
    // Get username from STDIN
    printf("Please enter username:\n");
    count = 0;
    while (count < CONFIG_USER_INPUT_LENGTH) {
        int c = fgetc(stdin);
        if (c == '\n' && count > 0) {
            buf[count] = '\0';
            break;
        } else if (c > 31 && c < 127) {
            buf[count] = c;
            ++count;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    mqtt_cfg.credentials.username = (char*)calloc(count+1,sizeof(char*));
    strncpy((char*)mqtt_cfg.credentials.username, buf, count+1);
            
    // Get password from STDIN
    printf("Please enter password:\n");
    count = 0;
    while (count < CONFIG_USER_INPUT_LENGTH) {
        int c = fgetc(stdin);
        if (c == '\n' && count > 0) {
            buf[count] = '\0';
            break;
        } else if (c > 31 && c < 127) {
            buf[count] = c;
            ++count;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    mqtt_cfg.credentials.authentication.password = (char*)calloc(count+1,sizeof(char*));
    strncpy((char*)mqtt_cfg.credentials.authentication.password, buf, count+1);
    
    // Configure the mqtt client
    ESP_LOGI(TAG,"Connecting to: %s\n with client-id: %s\n Username: %s\n Password: ****\n",
     mqtt_cfg.broker.address.uri,
     mqtt_cfg.credentials.client_id,
     mqtt_cfg.credentials.username);
    
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    if (mqtt_v5){
        esp_mqtt5_client_set_connect_property(client, &connect_property);
        ESP_LOGI(TAG,"Using MQTT protocol version 5.0");
    }
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    
    // Try to connect
    esp_mqtt_client_start(client);
    return ESP_OK;
}

esp_err_t sntp(void){
    ESP_LOGI(TAG, "Initializing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "0.de.pool.ntp.org");
    sntp_setservername(1, "1.de.pool.ntp.org");
    sntp_setservername(2, "2.de.pool.ntp.org");
    sntp_init();
    // Wait for time to be set
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    const int retry_count = 15;
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry <= retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(3000 / portTICK_PERIOD_MS);
    }
    time(&now);
    // Set timezone
    setenv("TZ", "EST-2", 1);
    tzset();
    localtime_r(&now, &timeinfo);
    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "The current date/time is: %s", strftime_buf);

    return ESP_OK;

}

// static void sds011_task(void *arg)
// {
//     /* Configure parameters of an UART driver,
//      * communication pins and install the driver */
//     uart_config_t uart_config = {
//         .baud_rate = 9600,
//         .data_bits = UART_DATA_8_BITS,
//         .parity    = UART_PARITY_DISABLE,
//         .stop_bits = UART_STOP_BITS_1,
//         .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
//         .source_clk = UART_SCLK_DEFAULT,
//     };
//     int intr_alloc_flags = 0;

// #if CONFIG_UART_ISR_IN_IRAM
//     intr_alloc_flags = ESP_INTR_FLAG_IRAM;
// #endif

//     ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, BUF_SIZE * 2, 0, 0, NULL, intr_alloc_flags));
//     ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
//     ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, TXD, RXD, RTS, CTS));

//     // Configure a temporary buffer for the incoming data
//     uint8_t *data = (uint8_t *) malloc(BUF_SIZE);

//     while (1) {
//         // Read data from the UART
//         int len = uart_read_bytes(UART_PORT_NUM, data, (BUF_SIZE - 1), 20 / portTICK_PERIOD_MS);
//         if (len) {
//             data[len] = '\0';
//             ESP_LOGI(TAG, "Recv str: %s", (char *) data);
//             uxTaskGetStackHighWaterMark(NULL);
//         }
//     }
// }

void data_task(void* pvParameters) {
  struct sds011_rx_packet rx_packet;
  TickType_t xLastWakeTime;
  xLastWakeTime = xTaskGetTickCount();
  for (;;) {
    /** Wake the sensor up. */
    sds011_send_cmd_to_queue(&sds011_tx_wakeup_packet, 0);

    /** Give it a few seconds to create some airflow. */
    vTaskDelay(pdMS_TO_TICKS(SDS011_ON_DURATION * 1000));

    /** Read the data (which is the latest when data queue size is 1). */
    if (sds011_recv_data_from_queue(&rx_packet, 0) == SDS011_OK) {
      float pm2_5;
      float pm10;

      pm2_5 = ((rx_packet.payload_query_data.pm2_5_high << 8) |
               rx_packet.payload_query_data.pm2_5_low) /
              10.0;
      pm10 = ((rx_packet.payload_query_data.pm10_high << 8) |
              rx_packet.payload_query_data.pm10_low) /
             10.0;

      printf(
          "PM2.5: %.2f\n"
          "PM10: %.2f\n",
          pm2_5, pm10);

      /** Set the sensor to sleep. */
      sds011_send_cmd_to_queue(&sds011_tx_sleep_packet, 0);

      /** Wait for next interval time. */
      vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(PRINT_INTERVAL * 1000));
    }
  }
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_ERROR);
    esp_log_level_set("network_connect", ESP_LOG_INFO);
    esp_log_level_set("network_common", ESP_LOG_INFO);
    esp_log_level_set("MQTTS_EXAMPLE", ESP_LOG_VERBOSE);
    


    
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    sds011_begin(SDS011_UART_PORT, SDS011_TX_GPIO, SDS011_RX_GPIO);
    assert(xTaskCreatePinnedToCore(data_task, "sds011", 2048, NULL, 0, NULL, 1) ==
         pdTRUE);
    // xTaskCreate(sds011_task, "uart_SDS011_task", SDS011_TASK_STACK_SIZE, NULL, 10, NULL);
    ESP_ERROR_CHECK(network_connect());
    ESP_ERROR_CHECK(sntp());
    ESP_ERROR_CHECK(mqtt_start());
}