#ifndef WIFI_COMPONENT_H
#define WIFI_COMPONENT_H

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"


#include "lwip/err.h"
#include "lwip/sys.h"
#include "nvs_component.h"
#include "ble_component.h"
#include "mqtt_component.h"

#define FACTORY_WIFI_SSID       "GIO"
#define FACTORY_WIFI_PSWD       "11001100"
#define MAX_RETRY       5
#define MAX_RETRY_RESET 45


#define WIFI_TAG "WIFI_COMPONENT"

void wifi_init_sta(void);
#endif // WIFI_COMPONENT_H  // End of the include guard