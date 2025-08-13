#ifndef MQTT_COMPONENT_H
#define MQTT_COMPONENT_H


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "cJSON.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"

#define MQTT_TAG "MQTT_COMPONENT"
#define BROKER  "10.1.24.1"
extern esp_mqtt_client_handle_t mqtt_client;
extern bool mqtt_status;

void mqtt_app_start(void);

void pgn_configure_handler(cJSON *payload);
void dnv_configure_handler(cJSON *payload);
#endif // MQTT_COMPONENT_H  // End of the include guard