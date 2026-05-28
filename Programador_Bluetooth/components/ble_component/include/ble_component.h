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
#include "sdkconfig.h"

#define BLE_TAG "BLE_COMPONENT"

#define PROFILE_NUM                 1
#define PROFILE_APP_IDX             0
#define ESP_APP_ID                  0x55
#define SVC_INST_ID                 0

#define GATTS_DEMO_CHAR_VAL_LEN_MAX 64
#define CHAR_DECLARATION_SIZE       (sizeof(uint8_t))

#define ADV_CONFIG_FLAG             (1 << 0)
#define SCAN_RSP_CONFIG_FLAG        (1 << 1)

// ── Variables compartidas con main_programador.c ──────────────────────────────
// Número de dolly pendiente de programar (se establece al escribir 0xA0B6).
// Se consume y resetea a 0 cuando llega el comando de jaula.
extern volatile uint16_t ble_pending_dolly;

// Flag: el reTerminal pidió leer la EEPROM del esclavo conectado.
// Se establece al escribir "READ" en la característica de jaula (0xA0B4).
extern volatile bool ble_cmd_leer;
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Inicializa y anuncia el servicio BLE GATT
void ble_init();

/// @brief Detiene y libera los recursos BLE
void ble_deinit();

/// @brief Envía una notificación de texto al cliente BLE conectado
/// @param mensaje Texto a enviar (máx. GATTS_DEMO_CHAR_VAL_LEN_MAX bytes)
void ble_enviar_status(const char* mensaje);

#endif // BLE_COMPONENT_H
