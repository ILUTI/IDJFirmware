// Codigo ESP32-C3 maestro IDJ
// Librerías de ESP-IDF y componentes
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
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
#include "esp_system.h"
#include "esp_task_wdt.h"

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
    uint8_t ausencias;
    bool presente;
} dispositivo_t;

static dispositivo_t dispositivos[MAX_DEVICES]; // asigno el tamaño del arreglo
static size_t num_dispositivos = 0; // contador de dispositivos registrados
static bool nvs_dirty = false;      // se activa cuando hay cambios pendientes de guardar

// normalización de ROM para formato consistente
void rom_to_string(uint64_t rom, char *output) {
    uint8_t *bytes = (uint8_t*)&rom;
    for (int i = 0; i < 8; i++) {
        sprintf(output + (i * 2), "%02X", bytes[i]); // <-- sin invertir
    }
    output[16] = '\0';
}

// Convierte el string del JSON de vuelta a uint64_t respetando el orden físico (Little-Endian)
uint64_t string_to_rom(const char *str) {
    uint64_t rom = 0;
    uint8_t *bytes = (uint8_t*)&rom;
    for (int i = 0; i < 8; i++) {
        // Leemos de 2 en 2 caracteres y los guardamos secuencialmente en la RAM
        sscanf(str + (i * 2), "%2hhx", &bytes[i]); 
    }
    return rom;
}

// También una función de validación del Family Code del DS2431
// El DS2431 siempre tiene Family Code = 0x2D en byte[0].
bool rom_es_ds2431(uint64_t rom) {
    uint8_t family = (uint8_t)(rom & 0xFF); // byte[0] en little-endian es el LSB
    return (family == 0x2D);
}

// Genera "T0603-xxxx" a partir del número de jaula
void generar_unidad(uint16_t jaula, char *out) {
    snprintf(out, 12, "T0603-%04d", jaula);
}
// -----------------------------------

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

                uint64_t rom = string_to_rom(rom_item->valuestring);
                dispositivos[num_dispositivos].rom = rom;
                rom_to_string(rom, dispositivos[num_dispositivos].rom_str);
                strncpy(dispositivos[num_dispositivos].unidad, unidad_item->valuestring, 11);
                dispositivos[num_dispositivos].unidad[11] = '\0'; // garantizar null-terminator
                dispositivos[num_dispositivos].asignado = (bool)asig_item->valueint;
                // --> NUEVA LÍNEA: Forzar que inicie como desconectada al arrancar
                dispositivos[num_dispositivos].presente = false;
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
    dispositivos[idx].ausencias = 0;
    dispositivos[idx].presente = true;


    rom_to_string(rom, dispositivos[idx].rom_str);
    dispositivos[idx].rom = rom;
    strcpy(dispositivos[idx].unidad, "");
    dispositivos[idx].asignado = false;

    

    ESP_LOGI("IDJ", "Nuevo dispositivo agregado: %s", dispositivos[idx].rom_str);
    num_dispositivos++;
    nvs_dirty = true;
}

#define SCAN_INTERVAL_MS      10000  // intervalo entre ciclos (ms)
#define AUSENCIAS_PARAR_PING      5  // 5 × 10s = 50s: dejar de gastar bus en Phase 1
#define AUSENCIAS_EVICTAR        18  // 18 × 10s = ~3min: borrar del registro y NVS
#define BUS_ERRORES_MAX           5  // reiniciar ESP32 tras 5 errores I2C/1-Wire consecutivos

// Función de escaneo. forzar_descubrimiento = true salta el contador de ciclos
// y ejecuta el Search ROM inmediatamente (se usa al arrancar sin dispositivos).
void escanear_dispositivos(ds2482_t *ds2482, bool forzar_descubrimiento) {
    static uint8_t ciclos_escaneo = 0;
    ciclos_escaneo++;

    ESP_LOGI(TAG, "--- Iniciando PING a jaulas conocidas ---");

    // =================================================================
    // FASE 1: PING LIVIANO A JAULAS CONOCIDAS (Se hace SIEMPRE)
    // Match ROM + Read Memory 4 bytes — mínimo indispensable en bus.
    // =================================================================
    for (size_t i = 0; i < num_dispositivos; i++) {
        esp_task_wdt_reset(); // scan largo con retries puede acercarse al timeout de 30s
        if (dispositivos[i].asignado && dispositivos[i].ausencias < AUSENCIAS_PARAR_PING) {
            ds2431_t esclavo_conocido = { .rom_code = dispositivos[i].rom };
            uint8_t buffer[4];

            esp_err_t err_ping = ds2431_read_memory(ds2482, &esclavo_conocido, 0x00, buffer, 4);

            if (err_ping == ESP_OK && buffer[0] == DS2431_MAGIC_BYTE0 && buffer[1] == DS2431_MAGIC_BYTE1) {
                dispositivos[i].presente = true;
                dispositivos[i].ausencias = 0;

                // Detectar si fue reprogramado por el Programador entre ciclos
                uint16_t num_jaula = (uint16_t)buffer[2] | ((uint16_t)buffer[3] << 8);
                char nueva_unidad[12];
                generar_unidad(num_jaula, nueva_unidad);
                if (strcmp(dispositivos[i].unidad, nueva_unidad) != 0) {
                    strncpy(dispositivos[i].unidad, nueva_unidad, 11);
                    dispositivos[i].unidad[11] = '\0';
                    nvs_dirty = true;
                    ESP_LOGW(TAG, "Unidad reprogramada: %s ahora es %s", dispositivos[i].rom_str, nueva_unidad);
                }

                ESP_LOGI(TAG, "PING OK: %s sigue conectada.", dispositivos[i].unidad);
            } else {
                if (dispositivos[i].ausencias < 250) dispositivos[i].ausencias++;
                if (dispositivos[i].ausencias >= 3) dispositivos[i].presente = false;
                ESP_LOGW(TAG, "PING FALLIDO: ROM %s no responde.", dispositivos[i].rom_str);
            }
            // Pausa entre pings: 50ms para bus de 80m (~4-8nF).
            // El bus debe llegar a Vcc estable antes del siguiente reset.
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    // =================================================================
    // FASE 2: DESCUBRIMIENTO (cada 3 ciclos = 30s, o forzado)
    // Se reduce de 6 a 3 para detectar esclavos nuevos más rápido.
    // forzar_descubrimiento se activa cuando no hay ningún dispositivo
    // registrado — así el primer esclavo conectado se detecta al instante.
    // =================================================================
    if (forzar_descubrimiento || ciclos_escaneo >= 3) {
        ciclos_escaneo = 0;
        esp_task_wdt_reset(); // Fase 2 puede tomar varios segundos con retries
        ESP_LOGI(TAG, "--- Iniciando DESCUBRIMIENTO de nuevas jaulas ---");

        uint64_t roms[MAX_DEVICES];
        size_t found = 0;

        esp_err_t err = ds2482_search_rom_all(roms, MAX_DEVICES, &found);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Search ROM reportó error. Jaulas encontradas: %zu", found);
        }

        for (size_t i = 0; i < found; i++) {
            if (!rom_es_ds2431(roms[i])) continue;

            char rom_str[17];
            rom_to_string(roms[i], rom_str);

            if (!existe_dispositivo_str(rom_str)) {
                // ── Dispositivo nuevo: agregar y leer EEPROM ──
                ESP_LOGI(TAG, "NUEVA JAULA DETECTADA! ROM: %s", rom_str);
                agregar_dispositivo(roms[i]);

                ds2431_t esclavo_nuevo = { .rom_code = roms[i] };
                ds2431_data_t datos_leidos;

                if (ds2431_leer_datos(ds2482, &esclavo_nuevo, &datos_leidos) == ESP_OK && datos_leidos.valido) {
                    for (size_t j = 0; j < num_dispositivos; j++) {
                        if (dispositivos[j].rom == roms[i]) {
                            strncpy(dispositivos[j].unidad, datos_leidos.unidad, 11);
                            dispositivos[j].unidad[11] = '\0';
                            dispositivos[j].asignado = true;
                            dispositivos[j].presente = true;
                            dispositivos[j].ausencias = 0;
                            break;
                        }
                    }
                }
            } else {
                // ── Dispositivo ya conocido ──
                for (size_t j = 0; j < num_dispositivos; j++) {
                    if (dispositivos[j].rom != roms[i]) continue;

                    if (!dispositivos[j].asignado) {
                        // Re-leer EEPROM por si el Programador lo grabó entre ciclos
                        ds2431_t esclavo = { .rom_code = roms[i] };
                        ds2431_data_t datos;
                        if (ds2431_leer_datos(ds2482, &esclavo, &datos) == ESP_OK && datos.valido) {
                            strncpy(dispositivos[j].unidad, datos.unidad, 11);
                            dispositivos[j].unidad[11] = '\0';
                            dispositivos[j].asignado = true;
                            dispositivos[j].presente = true;
                            dispositivos[j].ausencias = 0;
                            nvs_dirty = true;
                            ESP_LOGI(TAG, "Esclavo asignado: %s → %s", rom_str, datos.unidad);
                        }
                    } else if (dispositivos[j].ausencias > 0) {
                        // Estaba marcado ausente pero el Search ROM lo encontró: reconectado
                        dispositivos[j].presente = true;
                        dispositivos[j].ausencias = 0;
                        ESP_LOGI(TAG, "Jaula reconectada: %s", dispositivos[j].unidad);
                    }
                    break;
                }
            }
        }

        // ── Incrementar ausencias de dispositivos que Fase 2 tampoco encontró ──
        // Cubre el caso donde Fase 1 los saltó (ausencias >= AUSENCIAS_PARAR_PING)
        // y Search ROM tampoco los detecta: sin este bloque, ausencias se congela
        // en AUSENCIAS_PARAR_PING y el dispositivo fantasma nunca es evictado.
        for (size_t i = 0; i < num_dispositivos; i++) {
            if (dispositivos[i].ausencias < AUSENCIAS_PARAR_PING) continue; // Fase 1 ya lo manejó
            bool encontrado = false;
            for (size_t k = 0; k < found; k++) {
                if (dispositivos[i].rom == roms[k]) { encontrado = true; break; }
            }
            if (!encontrado) {
                if (dispositivos[i].ausencias < 250) dispositivos[i].ausencias++;
                if (dispositivos[i].ausencias >= 3) dispositivos[i].presente = false;
            }
        }

        // ── Evictar dispositivos ausentes demasiado tiempo ──
        // Se borran del array y de NVS. Si vuelven a conectarse en
        // otro viaje, Phase 2 los re-agrega leyendo la EEPROM de nuevo.
        size_t j = 0;
        while (j < num_dispositivos) {
            if (dispositivos[j].ausencias >= AUSENCIAS_EVICTAR) {
                ESP_LOGW(TAG, "Descartando jaula ausente: %s (%s)",
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

    // Guardar en flash solo si hubo cambios reales
    if (nvs_dirty) {
        guardar_en_nvs();
        nvs_dirty = false;
    }
}

// ============================================================
// App_main con ciclo de escaneo mejorado
// - Validación de Family Code en cada ROM detectado
// - Log estructurado con estado de cada dispositivo
// - Separación clara de lógica de escaneo en función propia
// ============================================================
// FUNCION MAIN --------------------------------------------------------------------------------------------
void app_main(void) {
    init_nvs_component();
    cargar_desde_nvs();

    wifi_init_sta();
    mqtt_app_start();

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
        ESP_LOGE(TAG, "DS2482 no detectado en bus I2C — reiniciando en 5s");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }
    ESP_LOGI(TAG, "DS2482 inicializado correctamente");

    // Activar Active Pullup en el DS2482 — crítico para cables largos
    // Sin esto, la señal se degrada en los ~15m de cable por jaula
    esp_err_t cfg_err = ds2482_configure(&ds2482, DS2482_CFG_APU);
    if (cfg_err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo configurar APU en DS2482 — señal puede degradarse en cables largos");
        // No retornamos, el sistema puede seguir funcionando sin APU
        // pero con mayor riesgo de errores en distancias largas
    }

    // Watchdog: reinicia el ESP32 si el bus se traba y no alimentamos el WDT en 30s.
    // Cubre el caso donde ds2482_busy_wait() se cuelga por I2C frozen o hardware fault.
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms    = 30000,   // 30s — cubre un ciclo de escaneo completo con margen
        .idle_core_mask = 0,
        .trigger_panic  = true,   // genera coredump + reset automático
    };
    if (esp_task_wdt_reconfigure(&wdt_cfg) == ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(esp_task_wdt_init(&wdt_cfg));
    }
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    ESP_LOGI(TAG, "Task WDT activo: timeout 30s");

    uint8_t errores_bus = 0;
    bool bus_estuvo_vacio = false;

while (1) {
        esp_task_wdt_reset();   // alimentar watchdog: si no llegamos aquí en 30s → reset

        bool presence = false;
        err = ds2482_1wire_reset(&presence);

        if (err != ESP_OK) {
            errores_bus++;
            ESP_LOGE(TAG, "Error en reset 1-Wire [%d/%d]", errores_bus, BUS_ERRORES_MAX);
            if (errores_bus >= BUS_ERRORES_MAX) {
                ESP_LOGE(TAG, "Bus 1-Wire irrecuperable — reiniciando ESP32...");
                esp_restart();
            }
        } else {
            errores_bus = 0;    // I2C + DS2482 respondieron: limpiar contador
            if (!presence) {
                bus_estuvo_vacio = true;
                ESP_LOGW(TAG, "Bus vacío — ninguna jaula conectada");
                for (size_t i = 0; i < num_dispositivos; i++) {
                    if (dispositivos[i].ausencias < 250) dispositivos[i].ausencias++;
                    if (dispositivos[i].ausencias >= 3) {
                        dispositivos[i].presente = false;
                    }
                }
            } else {
                // Forzar Search ROM si: no hay dispositivos registrados,
                // o el bus estuvo vacío (reconexión) — para redescubrir de inmediato.
                bool forzar = (num_dispositivos == 0) || bus_estuvo_vacio;
                bus_estuvo_vacio = false;
                escanear_dispositivos(&ds2482, forzar);
            }
        }

        // ====================================================
        // PANTALLA DEL CONDUCTOR (Datos en tiempo real)
        // ====================================================
        ESP_LOGI(TAG, "\n=============================================");
        ESP_LOGI(TAG, "    JAULAS ACTUALMENTE ENGANCHADAS AL CAMIÓN ");
        ESP_LOGI(TAG, "=============================================");
        
        int jaulas_enganchadas = 0;
        for (size_t i = 0; i < num_dispositivos; i++) {
            if (dispositivos[i].presente) {
                jaulas_enganchadas++;
                ESP_LOGI(TAG, "[ENGANCHADA] Unidad: %-12s | ROM: %s", 
                    dispositivos[i].asignado ? dispositivos[i].unidad : "SIN ASIGNAR",
                    dispositivos[i].rom_str);
            }
        }
        
        if (jaulas_enganchadas == 0) {
            ESP_LOGI(TAG, "    >>> CAMIÓN SIN JAULAS <<<");
        }
        ESP_LOGI(TAG, "=============================================\n");

        // ====================================================
        // PUBLICACIÓN MQTT HACIA LA RASPBERRY PI
        // ====================================================
        cJSON *json = cJSON_CreateObject();
        if (json != NULL) {
            // Creamos un arreglo JSON llamado "jaulas"
            cJSON *jaulas_array = cJSON_AddArrayToObject(json, "jaulas");

            // Recorremos la tabla local
            for (size_t i = 0; i < num_dispositivos; i++) {
                // SOLO agregamos las que están FÍSICAMENTE PRESENTES
                if (dispositivos[i].presente) {
                    cJSON *jaula_obj = cJSON_CreateObject();
                    
                    cJSON_AddStringToObject(jaula_obj, "rom", dispositivos[i].rom_str);
                    
                    // Si está asignada mandamos el código (ej. T0603-0230), si no, avisamos
                    if (dispositivos[i].asignado) {
                        cJSON_AddStringToObject(jaula_obj, "unidad", dispositivos[i].unidad);
                    } else {
                        cJSON_AddStringToObject(jaula_obj, "unidad", "SIN_ASIGNAR");
                    }
                    
                    cJSON_AddItemToArray(jaulas_array, jaula_obj);
                }
            }

            char *json_string = cJSON_PrintUnformatted(json);
            if (json_string != NULL) {
                int msg_id = esp_mqtt_client_publish(mqtt_client, "GIO/IDJ", json_string, 0, 0, 0);
                if (msg_id != -1) {
                    ESP_LOGI(TAG, "MQTT Publicado OK: %s", json_string);
                } else {
                    ESP_LOGE(TAG, "Error al publicar en MQTT");
                }
                free(json_string);
            } else {
                ESP_LOGE(TAG, "cJSON_PrintUnformatted falló — sin memoria");
            }
            cJSON_Delete(json);
        } else {
            ESP_LOGE(TAG, "Fallo al crear el objeto JSON para MQTT");
        }

        // Esperar el intervalo de tiempo antes del próximo escaneo
        vTaskDelay(pdMS_TO_TICKS(SCAN_INTERVAL_MS));

    } // <-- Cierre del while(1)
} // <-- Cierre del app_main