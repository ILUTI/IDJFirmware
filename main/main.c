// Codigo ESP32-C3 maestro IDJ
// Librerías de ESP-IDF y componentes
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
# include "nvs.h"
# include "nvs_flash.h"
#include "cJSON.h"

#include "nvs_component.h"
#include "mqtt_component.h"
#include "ble_component.h"
#include "wifi_component.h"

#include "driver/i2c.h"
#include "ds2482.h" // header del DS2482 para bridge y comunicación del maestro 1-wire
#include "ds2431.h" // header del DS2431 para lectura del rom y esclavo 1-wire

#define TAG "IDJ" // defino el tag del logger y dispositivo

// Configuración del bus I2C
#define I2C_MASTER_SCL_IO 5 // SCL IO4
#define I2C_MASTER_SDA_IO 4 // SDA IO5
#define I2C_MASTER_NUM I2C_NUM_0
#define I2C_MASTER_FREQ_HZ 100000 // frecuencia del canal I2C

// 1. Creación de la estructura de dispositivos, agregar unidad, rom, timestamp, y status actual del dispositivo.
#define MAX_DEVICES 20 // definir la cantidad maxima de dispositivos para guardar
typedef struct {
    uint64_t rom; // rom unica de cada ds2431
    char rom_str[17]; // representación en string del rom para JSON (16 caracteres + null terminator)
    char unidad[12];   // "T0603-xxxx" codigo para jaulas
    bool asignado; // indicador para saber si ya fue asignado o no.
} dispositivo_t;

static dispositivo_t dispositivos[MAX_DEVICES]; // asigno el tamaño del arreglo
static size_t num_dispositivos = 0; // contador de dispositivos registrados

// normalización de ROM para formato consistente
void rom_to_string(uint64_t rom, char *output) {
    uint8_t *bytes = (uint8_t*)&rom;
    for (int i = 0; i < 8; i++) {
        sprintf(output + (i * 2), "%02X", bytes[i]); // <-- sin invertir
    }
    output[16] = '\0';
}

// También una función de validación del Family Code del DS2431
// El DS2431 siempre tiene Family Code = 0x2B en byte[0].
bool rom_es_ds2431(uint64_t rom) {
    uint8_t family = (uint8_t)(rom & 0xFF); // byte[0] en little-endian es el LSB
    return (family == 0x2B);
}

// 2. Funciones para registro de dispositivos en la NVS, guardo y cargo el JSON
// Funcion para guardar en la NVS del esp32
void guardar_en_nvs() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error abriendo NVS para escritura: %s", esp_err_to_name(err));
        return;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *array = cJSON_AddArrayToObject(root, "devices");

    for (size_t i = 0; i < num_dispositivos; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "rom", dispositivos[i].rom_str);
        cJSON_AddStringToObject(obj, "unidad", dispositivos[i].unidad);
        cJSON_AddBoolToObject(obj, "asignado", dispositivos[i].asignado);
        cJSON_AddItemToArray(array, obj);
    }

    char *json_str = cJSON_PrintUnformatted(root); // <-- PrintUnformatted ahorra espacio en NVS
    if (json_str) {
        err = nvs_set_str(handle, "devices", json_str);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error guardando en NVS: %s", esp_err_to_name(err));
        }
        nvs_commit(handle);
        free(json_str);
    }

    cJSON_Delete(root);
    nvs_close(handle);
}

void cargar_desde_nvs() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error abriendo NVS para lectura: %s", esp_err_to_name(err));
        return;
    }

    size_t required_size = 0;
    err = nvs_get_str(handle, "devices", NULL, &required_size);

    if (err == ESP_OK && required_size > 0) {
        char *json_str = malloc(required_size);
        if (!json_str) {
            ESP_LOGE(TAG, "malloc falló al cargar NVS");
            nvs_close(handle);
            return;
        }

        nvs_get_str(handle, "devices", json_str, &required_size);
        cJSON *root = cJSON_Parse(json_str);

        if (root) { // <-- verificar que el JSON parseó correctamente
            cJSON *array = cJSON_GetObjectItem(root, "devices");
            num_dispositivos = 0;

            cJSON *item = NULL;
            cJSON_ArrayForEach(item, array) {
                if (num_dispositivos >= MAX_DEVICES) break;

                cJSON *rom_item    = cJSON_GetObjectItem(item, "rom");
                cJSON *unidad_item = cJSON_GetObjectItem(item, "unidad");
                cJSON *asig_item   = cJSON_GetObjectItem(item, "asignado");

                // Verificar que los campos existen antes de usarlos
                if (!rom_item || !unidad_item || !asig_item) continue;

                uint64_t rom = strtoull(rom_item->valuestring, NULL, 16);
                dispositivos[num_dispositivos].rom = rom;
                rom_to_string(rom, dispositivos[num_dispositivos].rom_str);
                strncpy(dispositivos[num_dispositivos].unidad, unidad_item->valuestring, 11);
                dispositivos[num_dispositivos].unidad[11] = '\0'; // garantizar null-terminator
                dispositivos[num_dispositivos].asignado = (bool)asig_item->valueint;

                num_dispositivos++;
            }
            cJSON_Delete(root);
        } else {
            ESP_LOGE(TAG, "JSON en NVS está corrupto, se descarta");
        }

        free(json_str);
    } else {
        ESP_LOGI(TAG, "NVS vacío, iniciando sin dispositivos previos");
    }

    nvs_close(handle);
}

// Funcion que me devuelve si existe o no dispositivo registrado con un rom específico, 
// evitando duplicados en la memoria. 
bool existe_dispositivo_str(const char *rom_str) {
    for (size_t i = 0; i < num_dispositivos; i++) {
        if (strcmp(dispositivos[i].rom_str, rom_str) == 0) {
            return true;
        }
    }
    return false;
}

// Funcion para agregar un nuevo dispositivo a la lista
void agregar_dispositivo(uint64_t rom) {
    if (num_dispositivos >= MAX_DEVICES) {
        printf("Lista llena\n");
        return;
    }

    size_t idx = num_dispositivos;

    rom_to_string(rom, dispositivos[idx].rom_str);
    dispositivos[idx].rom = rom;
    strcpy(dispositivos[idx].unidad, "");
    dispositivos[idx].asignado = false;

    

    printf("Nuevo dispositivo agregado: %s\n", dispositivos[idx].rom_str);
    ESP_LOGI("IDJ", "Nuevo dispositivo agregado: %s", dispositivos[idx].rom_str);

    num_dispositivos++;

    guardar_en_nvs();
}



#define SCAN_INTERVAL_MS 10000 // <-- única variable a modificar (milisegundos)

// ============================================================
// MEJORA 4: app_main con ciclo de escaneo mejorado
// - Validación de Family Code en cada ROM detectado
// - Log estructurado con estado de cada dispositivo
// - Separación clara de lógica de escaneo en función propia
// ============================================================

// Función de escaneo extraída del while para mayor claridad
void escanear_dispositivos(ds2482_t *ds2482) {
    uint64_t roms[MAX_DEVICES];
    size_t found = 0;

    esp_err_t err = ds2482_search_rom_all(roms, MAX_DEVICES, &found);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error en búsqueda 1-Wire");
        return;
    }

    ESP_LOGI(TAG, "Dispositivos encontrados en bus: %zu", found);

    for (size_t i = 0; i < found; i++) {
        // Validar que sea un DS2431 por su Family Code (0x2B)
        if (!rom_es_ds2431(roms[i])) {
            ESP_LOGW(TAG, "ROM 0x%016llX ignorado: Family Code no corresponde a DS2431", roms[i]);
            continue;
        }

        char rom_str[17];
        rom_to_string(roms[i], rom_str);
        // Ahora rom_str = "2B A1 3F 00 00 00 00 E4" en orden correcto

        if (!existe_dispositivo_str(rom_str)) {
            agregar_dispositivo(roms[i]);
        } else {
            ESP_LOGI(TAG, "ROM %s ya registrado", rom_str);
        }
    }
}

// FUNCION MAIN --------------------------------------------------------------------------------------------
void app_main(void) {
    init_nvs_component();
    cargar_desde_nvs();

    // wifi_init_sta();
    // mqtt_app_start();

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    i2c_param_config(I2C_MASTER_NUM, &conf);
    i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);

    ds2482_t ds2482;
    esp_err_t err = ds2482_init(&ds2482, I2C_MASTER_NUM, DS2482_I2C_ADDR);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DS2482 no detectado en bus I2C");
        return;
    }
    ESP_LOGI(TAG, "DS2482 inicializado correctamente");

    // Activar Active Pullup en el DS2482 — crítico para cables largos
    // Sin esto, la señal se degrada en los ~15m de cable por jaula
    ds2482_configure(&ds2482, DS2482_CFG_APU); // APU = Active Pullup

        ds2482_configure(&ds2482, DS2482_CFG_APU);

    // Sigue igual — ahora sí existe la función y la constante
    esp_err_t cfg_err = ds2482_configure(&ds2482, DS2482_CFG_APU);
    if (cfg_err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo configurar APU en DS2482 — señal puede degradarse en cables largos");
        // No retornamos, el sistema puede seguir funcionando sin APU
        // pero con mayor riesgo de errores en distancias largas
    }

    while (1) {
        bool presence = false;
        err = ds2482_1wire_reset(&presence);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error en reset 1-Wire");
        } else if (!presence) {
            ESP_LOGW(TAG, "Sin presencia 1-Wire — ninguna jaula conectada");
        } else {
            escanear_dispositivos(&ds2482);
        }

        // Tabla de estado actual
        ESP_LOGI(TAG, "===== TABLA DE JAULAS =====");
        for (size_t i = 0; i < num_dispositivos; i++) {
            ESP_LOGI(TAG, "ROM: %s | Unidad: %s | Asignado: %s",
                dispositivos[i].rom_str,
                dispositivos[i].unidad,
                dispositivos[i].asignado ? "SI" : "NO");
        }

        vTaskDelay(pdMS_TO_TICKS(SCAN_INTERVAL_MS));
    }
}