#include <stdio.h>
#include <string.h>
#include "mqtt_component.h"


esp_mqtt_client_handle_t mqtt_client;
bool mqtt_status = false;
bool first_time = false;
static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(MQTT_TAG, "Last error %s: 0x%x", message, error_code);
    }
}


/*
 * @brief Event handler registered to receive MQTT events
 *
 *  This function is called by the MQTT client event loop.
 *
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this example).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(MQTT_TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32 "", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(MQTT_TAG, "MQTT_EVENT_CONNECTED");
        mqtt_status = true;
        char topic[16];
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        sprintf(topic, "GIO/IDJ-%02X%02X/+/", mac[4], mac[5]);
        //Subscribe to device topic
        esp_mqtt_client_subscribe(client,topic,0);
        if (! first_time){
            //This is the first time
            //Publish some message to received configurations
            sprintf(topic, "GIO/IDJ-%02X%02X", mac[4], mac[5]);
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddBoolToObject(msg, "active", false);
            char *json_string = cJSON_Print(msg);
            esp_mqtt_client_publish(client, topic, json_string, 0, 1, 1);
            first_time = true;
            free(json_string);
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        if (mqtt_status == true){
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_DISCONNECTED, BUT DEVICE WAS CONNECTED. (Maybe Wifi error restart ESP)");
            esp_restart();
        }
        ESP_LOGI(MQTT_TAG, "MQTT_EVENT_DISCONNECTED");
        mqtt_status = false;
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(MQTT_TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(MQTT_TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(MQTT_TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(MQTT_TAG, "MQTT_EVENT_DATA");
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(MQTT_TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(MQTT_TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
            esp_mqtt_client_disconnect(client);
        }
        break;
    default:
        ESP_LOGI(MQTT_TAG, "Other event id:%d", event->event_id);
        break;
    }
}

/// @brief MQTT Start App 
void mqtt_app_start(void)
{
    //Create Unique ID, base of the MAC ADDR
    uint8_t mac[6];
    char id_string[9];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    sprintf(id_string, "IDJ-%02x%02X", mac[4], mac[5]);
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address = {
                .hostname = BROKER,
                .port = 1883,
                .transport = MQTT_TRANSPORT_OVER_TCP,
            },
            .verification = {
                .skip_cert_common_name_check = true,
                .use_global_ca_store = false,
            },
        },
        .credentials = {
            .username = "gio-ecosystem",
            .client_id = id_string,
            .authentication = {
                .password = "gio-device"
            },
        },
        .session = {
            .last_will = {
                .topic = "last/will/topic",
                .msg = id_string,
                .msg_len = sizeof(id_string),
                .qos = 1,
                .retain = 0,
            },
            .disable_clean_session = false,
            .keepalive = 30,
            .protocol_ver = MQTT_PROTOCOL_V_3_1_1,
        },
        .network = {
            .reconnect_timeout_ms = 5000,
            .timeout_ms = 5000,
            .disable_auto_reconnect = false,
        },
        .task = {
            .priority = 10,
            .stack_size = 4096*4,
        },
        .buffer = {
            .size = 1024*4,
            .out_size = 4096*4,
        },
        .outbox = {
            .limit = 8192,
        },
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}