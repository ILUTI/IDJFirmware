// IDJ Programador — Solo Jaulas
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/i2c.h"

#include "nvs_component.h"
#include "ble_component.h"
#include "ds2482.h"
#include "ds2431.h"

#define TAG         "IDJ-PROG"
#define I2C_SCL_IO  5
#define I2C_SDA_IO  4
#define I2C_NUM     I2C_NUM_0
#define I2C_FREQ_HZ 100000

QueueHandle_t cola_programacion_jaula;

// Genera "T0603-xxxx" a partir del número
void generar_unidad(uint16_t jaula, char *out) {
    snprintf(out, 12, "T0603-%04d", jaula);
}

// Volcado hexadecimal de la EEPROM
void imprimir_eeprom(uint8_t *raw, size_t len) {
    printf("\n  Addr  | 00 01 02 03 04 05 06 07\n");
    printf("  ------+------------------------\n");
    for (size_t i = 0; i < len; i += 8) {
        printf("  0x%02X  | ", (unsigned int)i);
        for (size_t j = i; j < i + 8 && j < len; j++)
            printf("%02X ", raw[j]);
        printf("\n");
    }
}

// Verificación física de la EEPROM tras la escritura
bool verificar_eeprom(ds2482_t *ds2482, ds2431_t *esclavo) {
    printf("\n--- Verificación EEPROM ---\n");
    uint8_t raw[DS2431_EEPROM_BUF_LEN];
    if (ds2431_read_memory(ds2482, esclavo, 0x00, raw, sizeof(raw)) != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo leer la memoria");
        return false;
    }
    imprimir_eeprom(raw, sizeof(raw));

    ds2431_data_t leido;
    esp_err_t err = ds2431_leer_datos(ds2482, esclavo, &leido);
    if (err != ESP_OK || !leido.valido) {
        if (raw[0] == 0xFF) ESP_LOGW(TAG, "EEPROM virgen");
        else                ESP_LOGE(TAG, "Datos corruptos");
        return false;
    }
    printf("  Magic   : OK\n");
    printf("  Jaula   : #%d\n",  leido.numero_jaula);
    printf("  Unidad  : '%s'\n", leido.unidad);
    printf("  CRC-16  : OK\n");
    ESP_LOGI(TAG, "✅ Jaula #%d verificada correctamente", leido.numero_jaula);
    return true;
}

// Ejecuta la lectura de EEPROM y envía resultado por BLE
void cmd_leer_eeprom(ds2482_t *ds2482) {
    bool presence = false;
    ds2482_1wire_reset(&presence);
    if (!presence) { ble_enviar_status("READ_ERROR:sin_esclavo"); return; }

    uint64_t roms[2]; size_t found = 0;
    ds2482_search_rom_all(roms, 2, &found);
    if (found != 1) { ble_enviar_status("READ_ERROR:bus_ocupado"); return; }

    ds2431_t esclavo = { .rom_code = roms[0] };
    ds2431_data_t datos;
    esp_err_t err = ds2431_leer_datos(ds2482, &esclavo, &datos);

    if (err == ESP_ERR_INVALID_CRC) {
        ble_enviar_status("READ_ERROR:datos_corruptos"); return;
    }
    if (err != ESP_OK || !datos.valido) {
        ble_enviar_status("READ:VIRGEN"); return;
    }
    char msg[48];
    snprintf(msg, sizeof(msg), "READ:jaula=%d", datos.numero_jaula);
    ble_enviar_status(msg);
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    cola_programacion_jaula = xQueueCreate(5, sizeof(uint16_t));
    ble_init();
    ESP_LOGI(TAG, "BLE iniciado");

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER, .sda_io_num = I2C_SDA_IO,
        .scl_io_num = I2C_SCL_IO, .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE, .master.clk_speed = I2C_FREQ_HZ,
    };
    i2c_param_config(I2C_NUM, &conf);
    i2c_driver_install(I2C_NUM, conf.mode, 0, 0, 0);

    ds2482_t ds2482;
    if (ds2482_init(&ds2482, I2C_NUM, DS2482_I2C_ADDR) != ESP_OK) {
        ESP_LOGE(TAG, "DS2482 no detectado"); return;
    }
    ds2482_configure(&ds2482, DS2482_CFG_APU);

    ESP_LOGI(TAG, "\n========================================");
    ESP_LOGI(TAG, "  IDJ PROGRAMADOR — Solo Jaulas");
    ESP_LOGI(TAG, "  1. Conecta el esclavo al bus 1-Wire");
    ESP_LOGI(TAG, "  2. Conecta el reTerminal por BLE");
    ESP_LOGI(TAG, "  3. Selecciona jaula y presiona PROGRAMAR");
    ESP_LOGI(TAG, "  4. Para leer → escribe 'READ' a 0xA0B4");
    ESP_LOGI(TAG, "========================================\n");

    while (1) {
        // ── Comando: LEER EEPROM ──────────────────────────────────────────
        if (ble_cmd_leer) {
            ble_cmd_leer = false;
            ESP_LOGI(TAG, "COMANDO: Leer EEPROM");
            cmd_leer_eeprom(&ds2482);
        }

        // ── Comando: PROGRAMAR JAULA ──────────────────────────────────────
        uint16_t nueva_jaula = 0;
        if (xQueueReceive(cola_programacion_jaula, &nueva_jaula,
                          pdMS_TO_TICKS(100)) == pdTRUE) {

            ESP_LOGI(TAG, "ORDEN: Programar Jaula #%d", nueva_jaula);

            // 1. Verificar presencia
            bool presence = false;
            ds2482_1wire_reset(&presence);
            if (!presence) {
                ble_enviar_status("ERROR:sin_esclavo"); continue;
            }

            // 2. Descubrir ROM (debe haber exactamente 1)
            uint64_t roms[2]; size_t found = 0;
            ds2482_search_rom_all(roms, 2, &found);
            if (found != 1) {
                ble_enviar_status("ERROR:bus_ocupado_o_vacio"); continue;
            }

            ds2431_t esclavo = { .rom_code = roms[0] };

            // 3. Leer estado anterior para el mensaje
            ds2431_data_t datos_prev;
            char anterior[16] = "NUEVA/VACIA";
            if (ds2431_leer_datos(&ds2482, &esclavo, &datos_prev) == ESP_OK
                && datos_prev.valido) {
                snprintf(anterior, sizeof(anterior), "#%d",
                         datos_prev.numero_jaula);
            }

            // 4. Preparar y escribir nuevos datos
            char unidad[12];
            generar_unidad(nueva_jaula, unidad);
            ds2431_data_t datos_nuevos = {
                .numero_jaula = nueva_jaula,
                .timestamp    = 0,
                .valido       = false,
            };
            strncpy(datos_nuevos.unidad, unidad, 11);
            datos_nuevos.unidad[11] = '\0';

            esp_err_t err = ds2431_escribir_datos(&ds2482, &esclavo, &datos_nuevos);

            // 5. Verificar y notificar
            if (err == ESP_OK && verificar_eeprom(&ds2482, &esclavo)) {
                char msg[64];
                snprintf(msg, sizeof(msg),
                         "OK![J:%s]->[J:#%d]", anterior, nueva_jaula);
                ESP_LOGI(TAG, "⭐ %s", msg);
                ble_enviar_status(msg);
            } else {
                ble_enviar_status("ERROR:fallo_grabado_fisico");
            }
        }
    }
}
