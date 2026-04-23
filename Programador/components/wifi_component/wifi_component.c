#include <stdio.h>
#include "wifi_component.h"


/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;

void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(WIFI_TAG, "Retry to connect to the AP, retry number = %d", s_retry_num);
        } else {
            //Enable BLE Functionality
            ESP_LOGI(WIFI_TAG, "MAX RETRY LIMIT REACHE");
            ble_init();
            vTaskDelay(pdMS_TO_TICKS(1000));
            s_retry_num++;
            esp_wifi_connect();
        }
        if (s_retry_num >= MAX_RETRY_RESET){
            ESP_LOGI(WIFI_TAG, "MAX RETRY LIMIT FOR RESET ESP, RESETING DEVICE...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
        ESP_LOGI(WIFI_TAG,"connect to the AP fail");
        
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(WIFI_TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        ble_deinit();
        if(! mqtt_status){
            esp_mqtt_client_reconnect(mqtt_client);
        }
        return;
    }
}


void wifi_init_sta(void)
{

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = FACTORY_WIFI_SSID,
            .password = FACTORY_WIFI_PSWD,
            /* Authmode threshold resets to WPA2 as default if password matches WPA2 standards (pasword len => 8).
             * If you want to connect the device to deprecated WEP/WPA networks, Please set the threshold value
             * to WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK and set the password with length and format matching to
             * WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK standards.
             */
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
            .sae_h2e_identifier = "",
        },
    };
    esp_err_t err;
    char wifi_ssid[32];
    char *var_name = "wifi_ssid";
    err = read_str_nvs(var_name, wifi_ssid, sizeof(wifi_ssid));
    if (err == ESP_OK){

        ESP_LOGI(WIFI_TAG, "Successfully read %s = %s", var_name, wifi_ssid);
        snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", wifi_ssid);

    }else{
        ESP_LOGE(WIFI_TAG, "Failed to read from NVS: %s", esp_err_to_name(err));
    }
    
    char wifi_pswd[32];
    var_name = "wifi_pswd";
    err = read_str_nvs(var_name, wifi_pswd, sizeof(wifi_pswd));
    if (err == ESP_OK){
        ESP_LOGI(WIFI_TAG, "Successfully read %s = %s", var_name, wifi_pswd);
        snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s", wifi_pswd);
    }else{
        ESP_LOGE(WIFI_TAG, "Failed to read from NVS: %s", esp_err_to_name(err));
    }

    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(WIFI_TAG, "wifi_init_sta finished.");

}