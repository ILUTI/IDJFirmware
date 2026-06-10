// IDJ Maestro v2 — con soporte de Dolly
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"

#include "nvs_component.h"
#include "mqtt_component.h"
#include "ble_component.h"
#include "wifi_component.h"

#include "driver/i2c.h"
#include "ds2482.h"
#include "ds2431.h"
#include "esp_system.h"
#include "esp_task_wdt.h"

#define TAG "IDJ"

// Configuración del bus I2C
#define I2C_MASTER_SCL_IO    5
#define I2C_MASTER_SDA_IO    4
#define I2C_MASTER_NUM       I2C_NUM_0
#define I2C_MASTER_FREQ_HZ   100000

// Parámetros de escaneo
#define MAX_DEVICES          20
#define AUSENCIAS_EVICTAR    5
#define SCAN_INTERVAL_MS     3000   // Ciclo base: 3 segundos
#define BUS_ERRORES_MAX      5
#define CICLOS_EEPROM        10     // 10 × 3s = 30s entre lecturas completas de EEPROM

// ── Estructura de dispositivo v2 (con Dolly) ─────────────────────────────────
typedef struct {
    uint64_t rom;
    char     rom_str[17];
    char     unidad[12];
    char     unidad_dolly[12];
    bool     tiene_dolly;
    bool     asignado;
    uint8_t  ausencias;
    bool     presente;
} dispositivo_t;

static dispositivo_t dispositivos[MAX_DEVICES];
static size_t num_dispositivos = 0;
static bool nvs_dirty = false;

// ── Utilidades de ROM ─────────────────────────────────────────────────────────
void rom_to_string(uint64_t rom, char *output) {
    uint8_t *bytes = (uint8_t *)&rom;
    for (int i = 0; i < 8; i++) sprintf(output + (i * 2), "%02X", bytes[i]);
    output[16] = '\0';
}

uint64_t string_to_rom(const char *str) {
    uint64_t rom = 0;
    uint8_t *bytes = (uint8_t *)&rom;
    for (int i = 0; i < 8; i++) sscanf(str + (i * 2), "%2hhx", &bytes[i]);
    return rom;
}

bool rom_es_ds2431(uint64_t rom) {
    return ((uint8_t)(rom & 0xFF) == 0x2D);
}

// ── NVS ───────────────────────────────────────────────────────────────────────
void guardar_en_nvs() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &handle);
    if (err != ESP_OK) { ESP_LOGE(TAG, "Error NVS: %s", esp_err_to_name(err)); return; }

    cJSON *root  = cJSON_CreateObject();
    cJSON *array = cJSON_AddArrayToObject(root, "devices");

    for (size_t i = 0; i < num_dispositivos; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "rom",          dispositivos[i].rom_str);
        cJSON_AddStringToObject(obj, "unidad",       dispositivos[i].unidad);
        cJSON_AddStringToObject(obj, "unidad_dolly", dispositivos[i].unidad_dolly);
        cJSON_AddBoolToObject  (obj, "tiene_dolly",  dispositivos[i].tiene_dolly);
        cJSON_AddBoolToObject  (obj, "asignado",     dispositivos[i].asignado);
        cJSON_AddItemToArray(array, obj);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        err = nvs_set_str(handle, "devices", json_str);
        if (err != ESP_OK) ESP_LOGE(TAG, "Error guardando NVS: %s", esp_err_to_name(err));
        nvs_commit(handle);
        free(json_str);
    }
    cJSON_Delete(root);
    nvs_close(handle);
}

void cargar_desde_nvs() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &handle);
    if (err != ESP_OK) { ESP_LOGE(TAG, "Error NVS: %s", esp_err_to_name(err)); return; }

    size_t required_size = 0;
    err = nvs_get_str(handle, "devices", NULL, &required_size);

    if (err == ESP_OK && required_size > 0) {
        char *json_str = malloc(required_size);
        if (!json_str) { nvs_close(handle); return; }

        nvs_get_str(handle, "devices", json_str, &required_size);
        cJSON *root = cJSON_Parse(json_str);

        if (root) {
            cJSON *array = cJSON_GetObjectItem(root, "devices");
            num_dispositivos = 0;

            cJSON *item = NULL;
            cJSON_ArrayForEach(item, array) {
                if (num_dispositivos >= MAX_DEVICES) break;
                cJSON *rom_item    = cJSON_GetObjectItem(item, "rom");
                cJSON *unidad_item = cJSON_GetObjectItem(item, "unidad");
                cJSON *asig_item   = cJSON_GetObjectItem(item, "asignado");
                if (!rom_item || !unidad_item || !asig_item) continue;

                size_t idx = num_dispositivos;
                uint64_t rom = string_to_rom(rom_item->valuestring);
                dispositivos[idx].rom = rom;
                rom_to_string(rom, dispositivos[idx].rom_str);
                strncpy(dispositivos[idx].unidad, unidad_item->valuestring, 11);
                dispositivos[idx].unidad[11] = '\0';
                dispositivos[idx].asignado   = (bool)asig_item->valueint;
                dispositivos[idx].presente   = false;
                dispositivos[idx].ausencias  = 0;

                cJSON *dolly_item       = cJSON_GetObjectItem(item, "unidad_dolly");
                cJSON *tiene_dolly_item = cJSON_GetObjectItem(item, "tiene_dolly");
                if (dolly_item && tiene_dolly_item) {
                    dispositivos[idx].tiene_dolly = (bool)tiene_dolly_item->valueint;
                    if (dispositivos[idx].tiene_dolly) {
                        strncpy(dispositivos[idx].unidad_dolly, dolly_item->valuestring, 11);
                        dispositivos[idx].unidad_dolly[11] = '\0';
                    } else {
                        memset(dispositivos[idx].unidad_dolly, 0, 12);
                    }
                } else {
                    dispositivos[idx].tiene_dolly = false;
                    memset(dispositivos[idx].unidad_dolly, 0, 12);
                }
                num_dispositivos++;
            }
            cJSON_Delete(root);
        } else {
            ESP_LOGE(TAG, "JSON en NVS corrupto, se descarta");
        }
        free(json_str);
    } else {
        ESP_LOGI(TAG, "NVS vacío, iniciando sin dispositivos previos");
    }
    nvs_close(handle);
}

// ── Registro de dispositivos ──────────────────────────────────────────────────
bool existe_dispositivo_str(const char *rom_str) {
    for (size_t i = 0; i < num_dispositivos; i++)
        if (strcmp(dispositivos[i].rom_str, rom_str) == 0) return true;
    return false;
}

void agregar_dispositivo(uint64_t rom) {
    if (num_dispositivos >= MAX_DEVICES) {
        ESP_LOGW(TAG, "Lista llena"); return;
    }
    size_t idx = num_dispositivos;
    dispositivos[idx].rom       = rom;
    dispositivos[idx].ausencias = 0;
    dispositivos[idx].presente  = true;
    dispositivos[idx].asignado  = false;
    dispositivos[idx].tiene_dolly = false;
    memset(dispositivos[idx].unidad,       0, 12);
    memset(dispositivos[idx].unidad_dolly, 0, 12);
    rom_to_string(rom, dispositivos[idx].rom_str);
    ESP_LOGI(TAG, "Nuevo dispositivo: %s", dispositivos[idx].rom_str);
    num_dispositivos++;
}

// ── Leer EEPROM de un dispositivo y asignar sus datos ────────────────────────
// Devuelve true si la lectura fue exitosa.
bool leer_eeprom_dispositivo(ds2482_t *ds2482, size_t idx) {
    ds2431_t esclavo = { .rom_code = dispositivos[idx].rom };
    ds2431_data_t datos;
    esp_err_t err = ds2431_leer_datos(ds2482, &esclavo, &datos);

    if (err == ESP_OK && datos.valido) {
        strncpy(dispositivos[idx].unidad, datos.unidad_jaula, 11);
        dispositivos[idx].unidad[11] = '\0';
        dispositivos[idx].tiene_dolly = datos.tiene_dolly;
        if (datos.tiene_dolly) {
            strncpy(dispositivos[idx].unidad_dolly, datos.unidad_dolly, 11);
            dispositivos[idx].unidad_dolly[11] = '\0';
        } else {
            memset(dispositivos[idx].unidad_dolly, 0, 12);
        }
        dispositivos[idx].asignado = true;
        nvs_dirty = true;
        ESP_LOGI(TAG, "EEPROM leída: %s → Jaula %s | Dolly %s",
                 dispositivos[idx].rom_str,
                 dispositivos[idx].unidad,
                 dispositivos[idx].tiene_dolly ? dispositivos[idx].unidad_dolly : "N/A");
        return true;

    } else if (err == ESP_ERR_INVALID_CRC) {
        ESP_LOGW(TAG, "Esclavo %s con formato antiguo — reprogramar",
                 dispositivos[idx].rom_str);
    } else {
        ESP_LOGW(TAG, "Esclavo %s sin datos aún", dispositivos[idx].rom_str);
    }
    return false;
}

// ── Escaneo del bus 1-Wire ────────────────────────────────────────────────────
//
// leer_eeprom = false → solo descubre ROMs y actualiza presencia (rápido, 3s)
// leer_eeprom = true  → además lee la EEPROM de todos los presentes (30s)
//
void escanear_dispositivos(ds2482_t *ds2482, bool leer_eeprom) {
    uint64_t roms[MAX_DEVICES];
    size_t found = 0;

    esp_err_t err = ds2482_search_rom_all(roms, MAX_DEVICES, &found);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error search_rom_all: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "ROMs en bus: %d | Lectura EEPROM: %s",
             found, leer_eeprom ? "SI" : "no");

    // ── Fase 1: Agregar ROMs nuevos ───────────────────────────────────────────
    for (size_t i = 0; i < found; i++) {
        if (!rom_es_ds2431(roms[i])) continue;
        char rom_str[17];
        rom_to_string(roms[i], rom_str);
        if (!existe_dispositivo_str(rom_str)) {
            agregar_dispositivo(roms[i]);
            nvs_dirty = true;
        }
    }

    // ── Fase 2: Actualizar presencia y (si aplica) leer EEPROM ───────────────
    for (size_t j = 0; j < num_dispositivos; j++) {
        bool encontrado = false;
        for (size_t i = 0; i < found; i++) {
            char rom_str[17];
            rom_to_string(roms[i], rom_str);
            if (strcmp(dispositivos[j].rom_str, rom_str) == 0) {
                encontrado = true;
                break;
            }
        }

        if (encontrado) {
            if (dispositivos[j].ausencias > 0)
                ESP_LOGI(TAG, "Jaula reconectada: %s", dispositivos[j].unidad[0]
                         ? dispositivos[j].unidad : dispositivos[j].rom_str);
            dispositivos[j].presente  = true;
            dispositivos[j].ausencias = 0;

            // Leer EEPROM si:
            //   - Es un ciclo de lectura completa (cada 30s), O
            //   - El dispositivo no tiene datos todavía
            if (leer_eeprom || !dispositivos[j].asignado) {
                leer_eeprom_dispositivo(ds2482, j);
                // Pausa entre lecturas — el bus largo necesita recuperarse
                vTaskDelay(pdMS_TO_TICKS(300));
            }
        } else {
            if (dispositivos[j].ausencias < 250) dispositivos[j].ausencias++;
            if (dispositivos[j].ausencias >= 3)  dispositivos[j].presente = false;
        }
    }

    // ── Fase 3: Evictar ausentes prolongados ─────────────────────────────────
    size_t j = 0;
    while (j < num_dispositivos) {
        if (dispositivos[j].ausencias >= AUSENCIAS_EVICTAR) {
            ESP_LOGW(TAG, "Evictando: %s (%s)",
                     dispositivos[j].unidad[0] ? dispositivos[j].unidad : "SIN_ASIGNAR",
                     dispositivos[j].rom_str);
            memmove(&dispositivos[j], &dispositivos[j + 1],
                    (num_dispositivos - j - 1) * sizeof(dispositivo_t));
            num_dispositivos--;
            nvs_dirty = true;
        } else {
            j++;
        }
    }
}

// ── Publicar estado por MQTT ──────────────────────────────────────────────────
void publicar_mqtt() {
    cJSON *json = cJSON_CreateObject();
    if (!json) { ESP_LOGE(TAG, "Fallo al crear JSON"); return; }

    cJSON *jaulas_array = cJSON_AddArrayToObject(json, "jaulas");

    for (size_t i = 0; i < num_dispositivos; i++) {
        if (!dispositivos[i].presente) continue;
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "rom", dispositivos[i].rom_str);
        if (dispositivos[i].asignado) {
            cJSON_AddStringToObject(obj, "unidad", dispositivos[i].unidad);
            cJSON_AddStringToObject(obj, "dolly",
                                    dispositivos[i].tiene_dolly
                                        ? dispositivos[i].unidad_dolly
                                        : "SIN_DOLLY");
        } else {
            cJSON_AddStringToObject(obj, "unidad", "SIN_ASIGNAR");
            cJSON_AddStringToObject(obj, "dolly",  "SIN_DOLLY");
        }
        cJSON_AddItemToArray(jaulas_array, obj);
    }

    char *json_str = cJSON_PrintUnformatted(json);
    int msg_id = esp_mqtt_client_publish(mqtt_client, "GIO/IDJ", json_str, 0, 0, 0);
    if (msg_id != -1) ESP_LOGI(TAG, "MQTT OK: %s", json_str);
    else              ESP_LOGE(TAG, "Error publicando MQTT");

    free(json_str);
    cJSON_Delete(json);
}

// ── App main ──────────────────────────────────────────────────────────────────
void app_main(void) {
    init_nvs_component();
    cargar_desde_nvs();

    wifi_init_sta();
    mqtt_app_start();

    i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = I2C_MASTER_SDA_IO,
        .scl_io_num       = I2C_MASTER_SCL_IO,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    i2c_param_config(I2C_MASTER_NUM, &conf);
    i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);

    ds2482_t ds2482;
    esp_err_t err = ds2482_init(&ds2482, I2C_MASTER_NUM, DS2482_I2C_ADDR);
    if (err != ESP_OK) { ESP_LOGE(TAG, "DS2482 no detectado"); return; }
    ESP_LOGI(TAG, "DS2482 OK");

    ds2482_configure(&ds2482, DS2482_CFG_APU);

    // Watchdog 30s
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms = 30000, .idle_core_mask = 0, .trigger_panic = true,
    };
    if (esp_task_wdt_reconfigure(&wdt_cfg) == ESP_ERR_INVALID_STATE)
        ESP_ERROR_CHECK(esp_task_wdt_init(&wdt_cfg));
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    // ── Descubrimiento inicial al arranque ────────────────────────────────────
    // Espera que el bus se estabilice antes del primer escaneo
    ESP_LOGI(TAG, "Estabilizando bus 1-Wire...");
    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGI(TAG, "=== DESCUBRIMIENTO INICIAL ===");
    bool presence_boot = false;
    ds2482_1wire_reset(&presence_boot);
    if (presence_boot) {
        escanear_dispositivos(&ds2482, true);  // Leer EEPROM completo al arranque
    } else {
        ESP_LOGW(TAG, "Bus vacío al arranque — esperando jaulas");
    }
    if (nvs_dirty) { guardar_en_nvs(); nvs_dirty = false; }
    publicar_mqtt();

    // ── Ciclo principal ───────────────────────────────────────────────────────
    uint32_t ciclo      = 0;
    uint8_t errores_bus = 0;

    while (1) {
        esp_task_wdt_reset();
        ciclo++;

        bool presence = false;
        err = ds2482_1wire_reset(&presence);

        if (err != ESP_OK) {
            errores_bus++;
            ESP_LOGE(TAG, "Error reset 1-Wire [%d/%d]", errores_bus, BUS_ERRORES_MAX);
            if (errores_bus >= BUS_ERRORES_MAX) {
                ESP_LOGE(TAG, "Bus irrecuperable — reiniciando");
                esp_restart();
            }
            vTaskDelay(pdMS_TO_TICKS(SCAN_INTERVAL_MS));
            continue;
        }

        errores_bus = 0;

        if (!presence) {
            ESP_LOGW(TAG, "Bus vacío");
            for (size_t i = 0; i < num_dispositivos; i++) {
                if (dispositivos[i].ausencias < 250) dispositivos[i].ausencias++;
                if (dispositivos[i].ausencias >= 3)  dispositivos[i].presente = false;
            }
        } else {
            // Cada 30s → escaneo completo con lectura de EEPROM
            // Cada 3s  → solo presencia y ROMs nuevos
            bool es_ciclo_eeprom = (ciclo % CICLOS_EEPROM == 0);
            if (es_ciclo_eeprom)
                ESP_LOGI(TAG, "=== ESCANEO COMPLETO (ciclo %lu) ===", ciclo);

            escanear_dispositivos(&ds2482, es_ciclo_eeprom);
        }

        if (nvs_dirty) { guardar_en_nvs(); nvs_dirty = false; }

        // ── Display consola ───────────────────────────────────────────────────
        ESP_LOGI(TAG, "============================================");
        ESP_LOGI(TAG, "   JAULAS ENGANCHADAS | ciclo=%lu", ciclo);
        ESP_LOGI(TAG, "============================================");
        int enganchadas = 0;
        for (size_t i = 0; i < num_dispositivos; i++) {
            if (!dispositivos[i].presente) continue;
            enganchadas++;
            if (dispositivos[i].asignado) {
                ESP_LOGI(TAG, "[OK] Jaula: %-12s | Dolly: %-12s | ROM: %s",
                         dispositivos[i].unidad,
                         dispositivos[i].tiene_dolly
                             ? dispositivos[i].unidad_dolly : "N/A",
                         dispositivos[i].rom_str);
            } else {
                ESP_LOGI(TAG, "[??] SIN ASIGNAR | ROM: %s", dispositivos[i].rom_str);
            }
        }
        if (enganchadas == 0) ESP_LOGI(TAG, "   >>> SIN JAULAS <<<");
        ESP_LOGI(TAG, "============================================\n");

        publicar_mqtt();

        vTaskDelay(pdMS_TO_TICKS(SCAN_INTERVAL_MS));
    }
}