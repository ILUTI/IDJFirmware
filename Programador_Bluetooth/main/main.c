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
#include "ble_component.h" // Incluimos el componente BLE
#include "ds2482.h"
#include "ds2431.h"

#define TAG              "IDJ-PROG"
#define I2C_SCL_IO       5
#define I2C_SDA_IO       4
#define I2C_NUM          I2C_NUM_0
#define I2C_FREQ_HZ      100000

// Cola global para recibir los comandos desde la interrupción del BLE
QueueHandle_t cola_programacion_jaula;

// Genera "T0603-xxxx" a partir del número
void generar_unidad(uint16_t jaula, char *out) {
    snprintf(out, 12, "T0603-%04d", jaula);
}

// ── Imprime los bytes crudos de la EEPROM en formato tabla ──────
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

// ── Lee y verifica la EEPROM del esclavo, muestra resultado ─────
bool verificar_eeprom(ds2482_t *ds2482, ds2431_t *esclavo) {
    printf("\n--- Verificacion EEPROM ---\n");
    uint8_t raw[DS2431_EEPROM_DATA_LEN];
    esp_err_t err = ds2431_read_memory(ds2482, esclavo, 0x00, raw, sizeof(raw));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ERROR de comunicación: No se pudo leer la memoria (%s)", esp_err_to_name(err));
        return false;
    }

    imprimir_eeprom_cruda(raw, sizeof(raw));

    ds2431_data_t leido;
    err = ds2431_leer_datos(ds2482, esclavo, &leido);

    if (err != ESP_OK || !leido.valido) {
        if (raw[0] == 0xFF && raw[1] == 0xFF) {
            ESP_LOGW(TAG, "Estado: EEPROM VIRGEN (Sin programar)");
        } else {
            uint16_t crc_calc = ds2431_crc16(raw, 20);
            uint16_t crc_stored = (uint16_t)raw[20] | ((uint16_t)raw[21] << 8);
            ESP_LOGE(TAG, "Estado: DATOS CORRUPTOS (Almacenado=0x%04X | Calculado=0x%04X)", crc_stored, crc_calc);
        }
        return false;
    }

    printf("  [0x00-0x01] Magic     : OK ('ID')\n");
    printf("  [0x02-0x03] Jaula     : #%d\n", leido.numero_jaula);
    printf("  [0x04-0x0F] Unidad    : '%s'\n", leido.unidad);
    printf("  [0x14-0x15] CRC-16    : OK (0x%04X)\n", ds2431_crc16(raw, 20));

    ESP_LOGI(TAG, "✅ RESULTADO: Jaula #%d verificada correctamente.", leido.numero_jaula);
    return true; 
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Crear la cola que conectará el BLE con el proceso principal
    cola_programacion_jaula = xQueueCreate(5, sizeof(uint16_t));

    // Iniciar BLE
    ble_init();
    ESP_LOGI(TAG, "Bluetooth Iniciado. Esperando comandos...");

    // Configurar I2C
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

    // Inicializar Bridge DS2482
    ds2482_t ds2482;
    if (ds2482_init(&ds2482, I2C_NUM, DS2482_I2C_ADDR) != ESP_OK) {
        ESP_LOGE(TAG, "Fallo crítico: DS2482 no detectado"); 
        return;
    }
    ds2482_configure(&ds2482, DS2482_CFG_APU);

    ESP_LOGI(TAG, "\n=== SISTEMA LISTO ===");
    ESP_LOGI(TAG, "1. Conecta el esclavo a programar.");
    ESP_LOGI(TAG, "2. Envía el número de jaula (texto) vía nRF Connect.");

    // Ciclo While Principal --------------------------------------------------------

    while (1) {
        uint16_t nueva_jaula = 0;
        
        // El ESP32 se queda suspendido aquí sin gastar CPU hasta que llegue un dato por BLE
if (xQueueReceive(cola_programacion_jaula, &nueva_jaula, portMAX_DELAY) == pdTRUE) {
            
            ESP_LOGI(TAG, "\n------------------------------------------------");
            ESP_LOGI(TAG, "🚀 ORDEN RECIBIDA: Programar Jaula #%d", nueva_jaula);
            
            // 1. Reset y Verificación de presencia
            bool presence = false;
            ds2482_1wire_reset(&presence);
            if (!presence) {
                ble_enviar_status("ERROR: No hay esclavo conectado");
                continue;
            }

            // 2. Descubrir ROM
            uint64_t roms[2];
            size_t found = 0;
            ds2482_search_rom_all(roms, 2, &found);

            if (found != 1) {
                ble_enviar_status("ERROR: Bus ocupado o multi-drop");
                continue;
            }

            ds2431_t esclavo = { .rom_code = roms[0] };
            
            // ==========================================================
            // 3. LEER ESTADO ANTERIOR Y PREPARAR NOTIFICACIÓN
            // ==========================================================
            ds2431_data_t datos_previos;
            char info_anterior[32]; // Para guardar el texto del estado viejo
            
            esp_err_t err_read = ds2431_leer_datos(&ds2482, &esclavo, &datos_previos);
            
            if (err_read == ESP_OK && datos_previos.valido) {
                // El esclavo ya tenía un número
                snprintf(info_anterior, sizeof(info_anterior), "#%d", datos_previos.numero_jaula);
                ESP_LOGW(TAG, "Cambiando de Jaula %s a #%d", info_anterior, nueva_jaula);
            } else {
                // El esclavo estaba vacío o corrupto
                strcpy(info_anterior, "NUEVA/VACIA");
                ESP_LOGI(TAG, "Esclavo sin asignar. Asignando #%d", nueva_jaula);
            }

            // 4. Preparar y Escribir nuevos datos
            char unidad[12];
            generar_unidad(nueva_jaula, unidad);
            ds2431_data_t datos_nuevos = {
                .numero_jaula = nueva_jaula,
                .timestamp    = 0,
                .valido       = false,
            };
            strncpy(datos_nuevos.unidad, unidad, 11);
            datos_nuevos.unidad[11] = '\0';

            esp_err_t err_write = ds2431_escribir_datos(&ds2482, &esclavo, &datos_nuevos);
            
            // 5. Verificación física y Envío de Status por BLE
            if (err_write == ESP_OK && verificar_eeprom(&ds2482, &esclavo)) {
                
                // CONSTRUIR MENSAJE FINAL PARA EL CELULAR
                char msg_final[64];
                snprintf(msg_final, sizeof(msg_final), "OK! [%s] -> [#%d]", info_anterior, nueva_jaula);
                
                ESP_LOGI(TAG, "⭐⭐ %s ⭐⭐", msg_final);
                
                // ENVIAR AL CELULAR
                ble_enviar_status(msg_final); 

            } else {
                ble_enviar_status("ERROR: Fallo en grabado físico");
            }
        }
    }
}