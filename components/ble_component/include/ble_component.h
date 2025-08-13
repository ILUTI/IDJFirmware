#ifndef BLE_COMPONENT_H
#define BLE_COMPONENT_H

#include <string.h>
#include "esp_err.h"
#include "esp_system.h"
#include "esp_log.h"


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_mac.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"

#include "svc_table.h"

#include "nvs_component.h"

#define BLE_TAG "BLE_COMPONENT"

#define PROFILE_NUM                 1
#define PROFILE_APP_IDX             0
#define ESP_APP_ID                  0x55
#define SAMPLE_DEVICE_NAME          "IDJ"
#define SVC_INST_ID                 0

#define GATTS_DEMO_CHAR_VAL_LEN_MAX 64
#define CHAR_DECLARATION_SIZE       (sizeof(uint8_t))

#define ADV_CONFIG_FLAG             (1 << 0)
#define SCAN_RSP_CONFIG_FLAG        (1 << 1)

/// @brief Enable Bluetooth and advertisement BLE Service
void ble_init();

/// @brief Disable Bluetooth and advertisement BLE Service
void ble_deinit();

#endif // BLE_COMPONENT_H  // End of the include guard