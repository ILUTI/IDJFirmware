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

// Cola para recibir órdenes de programación de jaula desde BLE
QueueHandle_t cola_programacion_jaula;

// ── Generadores de código de unidad ──────────────────────────────────────────
void generar_unidad_jaula(uint16_t jaula, char *out) {
    snprintf(out, 12, "T0603-%04d", jaula);
}

void generar_unidad_dolly(uint16_t dolly, char *out) {
    snprintf(out, 12, "T0605-%04d", dolly);
}

// ── Imprime volcado hexadecimal de la EEPROM ──────────────────────────────────
void imprimir_eeprom_cruda(uint8_t *raw, size_t len) {
    printf("\n  Addr  | 00 01 02 03 04 05 06 07\n");
    printf("  ------+------------------------\n");
    for (size_t i = 0; i < len; i += 8) {
        printf("  0x%02X  | ", (unsigned int)i);
        for (size_t j = i; j < i + 8 && j < len; j++) {
            printf("%02X ", raw[j]);
        }
        printf("\n");
    }
}

// ── Lee y verifica la EEPROM, muestra resultado detallado ─────────────────────
bool verificar_eeprom(ds2482_t *ds2482, ds2431_t *esclavo) {
    printf("\n--- Verificación EEPROM (v2) ---\n");

    uint8_t raw[DS2431_EEPROM_BUF_LEN];
    esp_err_t err = ds2431_read_memory(ds2482, esclavo, 0x00, raw, sizeof(raw));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ERROR: No se pudo leer la memoria (%s)", esp_err_to_name(err));
        return false;
    }

    imprimir_eeprom_cruda(raw, sizeof(raw));

    ds2431_data_t leido;
    err = ds2431_leer_datos(ds2482, esclavo, &leido);

    if (err != ESP_OK || !leido.valido) {
        if (raw[0] == 0xFF && raw[1] == 0xFF) {
            ESP_LOGW(TAG, "Estado: EEPROM VIRGEN (sin programar)");
        } else if (err == ESP_ERR_INVALID_CRC) {
            // El mensaje ya fue emitido en ds2431_leer_datos
        } else {
            ESP_LOGE(TAG, "Estado: DATOS INVÁLIDOS");
        }
        return false;
    }

    printf("  [0x00-0x01] Magic      : OK ('ID')\n");
    printf("  [0x02-0x03] Jaula      : #%d\n",   leido.numero_jaula);
    printf("  [0x04-0x0F] Unidad     : '%s'\n",  leido.unidad_jaula);
    printf("  [0x10-0x11] Dolly      : %s\n",
           leido.tiene_dolly ? "presente" : "sin dolly");
    if (leido.tiene_dolly) {
        printf("  [0x10-0x11] Num. Dolly : #%d\n",  leido.numero_dolly);
        printf("  [0x12-0x1D] ID Dolly   : '%s'\n", leido.unidad_dolly);
    }
    printf("  [0x24-0x25] CRC-16     : OK (0x%04X)\n",
           ds2431_crc16(raw, DS2431_CRC_DATA_LEN));

    ESP_LOGI(TAG, "✅ Jaula #%d verificada correctamente.", leido.numero_jaula);
    return true;
}

// ── Ejecuta lectura de EEPROM y envía resultado por BLE ───────────────────────
void cmd_leer_eeprom(ds2482_t *ds2482) {
    bool presence = false;
    ds2482_1wire_reset(&presence);
    if (!presence) {
        ble_enviar_status("READ_ERROR:sin_esclavo");
        return;
    }

    uint64_t roms[2];
    size_t found = 0;
    ds2482_search_rom_all(roms, 2, &found);

    if (found != 1) {
        ble_enviar_status("READ_ERROR:bus_ocupado");
        return;
    }

    ds2431_t esclavo = { .rom_code = roms[0] };
    ds2431_data_t datos;
    esp_err_t err = ds2431_leer_datos(ds2482, &esclavo, &datos);

    if (err == ESP_ERR_INVALID_CRC) {
        ble_enviar_status("READ_ERROR:formato_antiguo");
        return;
    }

    if (err != ESP_OK || !datos.valido) {
        ble_enviar_status("READ:VIRGEN");
        return;
    }

    // Formato: "READ:jaula=230,dolly=45" o "READ:jaula=230,dolly=NONE"
    char msg[80];
    if (datos.tiene_dolly) {
        snprintf(msg, sizeof(msg), "READ:jaula=%d,dolly=%d",
                 datos.numero_jaula, datos.numero_dolly);
    } else {
        snprintf(msg, sizeof(msg), "READ:jaula=%d,dolly=NONE",
                 datos.numero_jaula);
    }
    ble_enviar_status(msg);
}

// ── App main ──────────────────────────────────────────────────────────────────
void app_main(void) {
    // Inicializar NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Crear cola de comandos de programación
    cola_programacion_jaula = xQueueCreate(5, sizeof(uint16_t));

    // Iniciar BLE
    ble_init();
    ESP_LOGI(TAG, "BLE iniciado. Esperando conexión del reTerminal...");

    // Configurar bus I2C
    i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = I2C_SDA_IO,
        .scl_io_num       = I2C_SCL_IO,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };
    i2c_param_config(I2C_NUM, &conf);
    i2c_driver_install(I2C_NUM, conf.mode, 0, 0, 0);

    // Inicializar bridge DS2482
    ds2482_t ds2482;
    if (ds2482_init(&ds2482, I2C_NUM, DS2482_I2C_ADDR) != ESP_OK) {
        ESP_LOGE(TAG, "Fallo crítico: DS2482 no detectado");
        return;
    }
    ds2482_configure(&ds2482, DS2482_CFG_APU);

    ESP_LOGI(TAG, "\n========================================");
    ESP_LOGI(TAG, "  IDJ PROGRAMADOR v2 — LISTO");
    ESP_LOGI(TAG, "  1. Conectar esclavo al bus 1-Wire");
    ESP_LOGI(TAG, "  2. Conectar reTerminal por BLE");
    ESP_LOGI(TAG, "  3. (Opcional) Enviar número de dolly → char 0xA0B6");
    ESP_LOGI(TAG, "  4. Enviar número de jaula → char 0xA0B4");
    ESP_LOGI(TAG, "  5. Para leer EEPROM → escribir 'READ' → char 0xA0B4");
    ESP_LOGI(TAG, "========================================\n");

    // ── Ciclo principal ───────────────────────────────────────────────────────
    while (1) {

        // ── Comando: LEER EEPROM ──────────────────────────────────────────────
        // Activado cuando el reTerminal escribe "READ" en la char 0xA0B4
        if (ble_cmd_leer) {
            ble_cmd_leer = false;
            ESP_LOGI(TAG, "COMANDO: Leer EEPROM del esclavo conectado");
            cmd_leer_eeprom(&ds2482);
        }

        // ── Comando: PROGRAMAR JAULA (+ DOLLY opcional) ───────────────────────
        // Activado cuando el reTerminal escribe un número en la char 0xA0B4.
        // El dolly pendiente (char 0xA0B6) se consume aquí y se resetea a 0.
        uint16_t nueva_jaula = 0;
        if (xQueueReceive(cola_programacion_jaula, &nueva_jaula,
                          pdMS_TO_TICKS(100)) == pdTRUE) {

            // Capturar y consumir el dolly pendiente atómicamente
            uint16_t nuevo_dolly  = ble_pending_dolly;
            ble_pending_dolly     = 0;
            bool     tiene_dolly  = (nuevo_dolly > 0);

            ESP_LOGI(TAG, "\n------------------------------------------------");
            ESP_LOGI(TAG, "🚀 ORDEN: Programar Jaula #%d%s%s",
                     nueva_jaula,
                     tiene_dolly ? " + Dolly #" : "",
                     tiene_dolly ? (char[]){nuevo_dolly / 1000 + '0',
                                            (nuevo_dolly % 1000) / 100 + '0',
                                            (nuevo_dolly % 100) / 10 + '0',
                                             nuevo_dolly % 10 + '0', '\0'} : "");

            // 1. Verificar presencia del esclavo
            bool presence = false;
            ds2482_1wire_reset(&presence);
            if (!presence) {
                ble_enviar_status("ERROR:sin_esclavo");
                continue;
            }

            // 2. Descubrir ROM (debe haber exactamente 1 esclavo)
            uint64_t roms[2];
            size_t found = 0;
            ds2482_search_rom_all(roms, 2, &found);
            if (found != 1) {
                ble_enviar_status("ERROR:bus_ocupado_o_vacio");
                continue;
            }

            ds2431_t esclavo = { .rom_code = roms[0] };

            // 3. Leer estado anterior para el mensaje de confirmación
            ds2431_data_t datos_previos;
            char info_anterior_jaula[16] = "NUEVA/VACIA";
            char info_anterior_dolly[16] = "NONE";

            esp_err_t err_read = ds2431_leer_datos(&ds2482, &esclavo, &datos_previos);
            if (err_read == ESP_OK && datos_previos.valido) {
                snprintf(info_anterior_jaula, sizeof(info_anterior_jaula),
                         "#%d", datos_previos.numero_jaula);
                if (datos_previos.tiene_dolly) {
                    snprintf(info_anterior_dolly, sizeof(info_anterior_dolly),
                             "#%d", datos_previos.numero_dolly);
                }
                ESP_LOGW(TAG, "Esclavo ya tenía: Jaula %s | Dolly %s",
                         info_anterior_jaula, info_anterior_dolly);
            } else {
                ESP_LOGI(TAG, "Esclavo virgen o formato antiguo — asignando nuevos datos");
            }

            // 4. Preparar nuevos datos
            char unidad_jaula[12];
            char unidad_dolly[12] = {0};
            generar_unidad_jaula(nueva_jaula, unidad_jaula);
            if (tiene_dolly) generar_unidad_dolly(nuevo_dolly, unidad_dolly);

            ds2431_data_t datos_nuevos = {
                .numero_jaula = nueva_jaula,
                .numero_dolly = nuevo_dolly,
                .tiene_dolly  = tiene_dolly,
                .timestamp    = 0,  // Sin RTC disponible en el programador
                .valido       = false,
            };
            strncpy(datos_nuevos.unidad_jaula, unidad_jaula, 11);
            datos_nuevos.unidad_jaula[11] = '\0';
            if (tiene_dolly) {
                strncpy(datos_nuevos.unidad_dolly, unidad_dolly, 11);
                datos_nuevos.unidad_dolly[11] = '\0';
            }

            // 5. Escribir EEPROM
            esp_err_t err_write = ds2431_escribir_datos(&ds2482, &esclavo, &datos_nuevos);

            // 6. Verificar y notificar resultado
            if (err_write == ESP_OK && verificar_eeprom(&ds2482, &esclavo)) {
                char msg[80];
                if (tiene_dolly) {
                    snprintf(msg, sizeof(msg),
                             "OK![J:%s D:%s]->[J:#%d D:#%d]",
                             info_anterior_jaula, info_anterior_dolly,
                             nueva_jaula, nuevo_dolly);
                } else {
                    snprintf(msg, sizeof(msg),
                             "OK![J:%s D:%s]->[J:#%d D:NONE]",
                             info_anterior_jaula, info_anterior_dolly,
                             nueva_jaula);
                }
                ESP_LOGI(TAG, "⭐ %s", msg);
                ble_enviar_status(msg);
            } else {
                ble_enviar_status("ERROR:fallo_grabado_fisico");
            }
        }
    } // while(1)
}
