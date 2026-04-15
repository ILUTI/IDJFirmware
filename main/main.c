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
    char unidad[12];   // "T0603-xxxx" codigo para jaulas
    bool asignado; // indicador para saber si ya fue asignado o no.
} dispositivo_t;

static dispositivo_t dispositivos[MAX_DEVICES]; // asigno el tamaño del arreglo
static size_t num_dispositivos = 0; // contador de dispositivos registrados

// 2. Funciones para registro de dispositivos en la NVS, guardo y cargo el JSON
// Funcion para guardar en la NVS del esp32
void guardar_en_nvs() {
    nvs_handle_t handle;
    nvs_open("storage", NVS_READWRITE, &handle);

    cJSON *root = cJSON_CreateObject();
    cJSON *array = cJSON_AddArrayToObject(root, "devices");

    for (size_t i = 0; i < num_dispositivos; i++) {
        cJSON *obj = cJSON_CreateObject();

        char rom_str[17];
        sprintf(rom_str, "%016llX", dispositivos[i].rom);

        cJSON_AddStringToObject(obj, "rom", rom_str);
        cJSON_AddStringToObject(obj, "unidad", dispositivos[i].unidad);
        cJSON_AddBoolToObject(obj, "asignado", dispositivos[i].asignado);

        cJSON_AddItemToArray(array, obj);
    }

    char *json_str = cJSON_Print(root);

    nvs_set_str(handle, "devices", json_str);
    nvs_commit(handle);
    nvs_close(handle);

    free(json_str);
    cJSON_Delete(root);
}

// Funcion para cargar desde
void cargar_desde_nvs() {
    nvs_handle_t handle;
    nvs_open("storage", NVS_READWRITE, &handle);

    size_t required_size = 0;

    if (nvs_get_str(handle, "devices", NULL, &required_size) == ESP_OK) {
        char *json_str = malloc(required_size);
        nvs_get_str(handle, "devices", json_str, &required_size);

        cJSON *root = cJSON_Parse(json_str);
        cJSON *array = cJSON_GetObjectItem(root, "devices");

        num_dispositivos = 0;

        cJSON *item = NULL;
        cJSON_ArrayForEach(item, array) {
            const char *rom_str = cJSON_GetObjectItem(item, "rom")->valuestring;

            uint64_t rom = strtoull(rom_str, NULL, 16);

            dispositivos[num_dispositivos].rom = rom;
            strcpy(dispositivos[num_dispositivos].unidad,
                   cJSON_GetObjectItem(item, "unidad")->valuestring);
            dispositivos[num_dispositivos].asignado =
                   cJSON_GetObjectItem(item, "asignado")->valueint;

            num_dispositivos++;
        }

        cJSON_Delete(root);
        free(json_str);
    }

    nvs_close(handle);
}

// Funcion que me devuelve si existe o no dispositivo registrado con un rom específico, 
// evitando duplicados en la memoria. 
bool existe_dispositivo(uint64_t rom) {
    for (size_t i = 0; i < num_dispositivos; i++) {
        if (dispositivos[i].rom == rom) {
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

    dispositivos[num_dispositivos].rom = rom;
    strcpy(dispositivos[num_dispositivos].unidad, "");
    dispositivos[num_dispositivos].asignado = false;

    num_dispositivos++;

    printf("Nuevo dispositivo agregado: %016llX\n", rom); // formato hexadecimal para el rom y para long long (64 bits)

    guardar_en_nvs();  // persistencia inmediata
}


// FUNCION MAIN --------------------------------------------------------------------------------------------
void app_main(void) {

    //Initialize NVS
    init_nvs_component();

    // Cargo los dipsositivos desde la NVS
    cargar_desde_nvs();

    //Initialize Wifi Station
    //wifi_init_sta();

    //Initialize MQTT Client
    //mqtt_app_start();

    // Configuración de I2C
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

    // Inicialización del DS2482 y detección del dispositivo I2C
    ds2482_t ds2482;
    esp_err_t err = ds2482_init(&ds2482, I2C_MASTER_NUM, DS2482_I2C_ADDR);
    if (err != ESP_OK) {
        printf("No se detecta el DS2482 en el bus I2C.\n");
        return;
    }
    else {
        printf("DS2482 detectado correctamente en el bus I2C.\n");
    }

    uint8_t status = 0;
    err = ds2482_read_status(&ds2482, &status);
    if (err != ESP_OK) {
        printf("Error leyendo el registro de estado del DS2482.\n");
        return;
    }
    printf("Registro de estado DS2482: 0x%02X\n", status);

    // Detección de la presencia del dispositivo 1-Wire
    bool presence = false;
    err = ds2482_1wire_reset(&presence);
    if (err != ESP_OK) {
        printf("Error en reset 1-Wire\n");
        return;
    }
    if (presence) {
        printf("Dispositivo 1-Wire detectado (presencia=1)\n");
    } else {
        printf("No se detectó dispositivo 1-Wire (presencia=0)\n");
    }

    // ------------------------------ Para Pruebas, detectar un dispositivo 1-Wire y mostrar su ROM ------------------------------
    //Detecta solo un dispositivo 1-Wire
    uint64_t rom_code = 0;
    err = ds2482_search_rom(&rom_code);
    if (err == ESP_OK) {
        printf("Dirección ROM del dispositivo 1-Wire: 0x%016llX\n", rom_code);
    } else {
        printf("Error buscando la dirección ROM del dispositivo 1-Wire.\n");
    }

    //Escaneo de ROM's
    while (1)
    {
    uint64_t roms[5];
    size_t found = 0;

    err = ds2482_search_rom_all(roms, 5, &found);

    if (err == ESP_OK) {
        printf("Se encontraron %zu dispositivos:\n", found);

        for (size_t i = 0; i < found; i++) {

            printf("ROM: %016llX\n", roms[i]);

            if (!existe_dispositivo(roms[i])) {
                agregar_dispositivo(roms[i]);
            }
        }
    }

    // Mostrar estado actual
    printf("----- TABLA LOCAL -----\n");
    for (size_t i = 0; i < num_dispositivos; i++) {
        printf("ROM: %016llX | Unidad: %s | Asignado: %d\n",
               dispositivos[i].rom,
               dispositivos[i].unidad,
               dispositivos[i].asignado);
    }

    vTaskDelay(pdMS_TO_TICKS(5000));
    }
}