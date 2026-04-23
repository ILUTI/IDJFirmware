#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "ds2482.h"
#include "esp_err.h"

// Comandos ROM 1-Wire
#define OW_CMD_MATCH_ROM        0x55
#define OW_CMD_SKIP_ROM         0xCC

// Comandos de memoria DS2431
#define DS2431_CMD_WRITE_SCRATCHPAD  0x0F
#define DS2431_CMD_READ_SCRATCHPAD   0xAA
#define DS2431_CMD_COPY_SCRATCHPAD   0x55
#define DS2431_CMD_READ_MEMORY       0xF0

// Mapa de EEPROM IDJ
#define DS2431_ADDR_MAGIC       0x00  // 2 bytes: 0x49 0x44 ("ID")
#define DS2431_ADDR_JAULA       0x02  // 2 bytes: uint16_t número de jaula (1-250)
#define DS2431_ADDR_UNIDAD      0x04  // 12 bytes: "T0603-xxxx\0"
#define DS2431_ADDR_TIMESTAMP   0x10  // 4 bytes: unix time uint32_t
#define DS2431_ADDR_CRC16       0x14  // 2 bytes: CRC-16 de bytes 0x00-0x13
#define DS2431_EEPROM_DATA_LEN  0x16  // 22 bytes de datos totales con CRC

#define DS2431_MAGIC_BYTE0      0x49  // 'I'
#define DS2431_MAGIC_BYTE1      0x44  // 'D'

// Tiempo de escritura EEPROM según datasheet DS2431 (máx 10ms por página)
#define DS2431_COPY_SCRATCHPAD_DELAY_MS  10

typedef struct {
    uint64_t rom_code;
} ds2431_t;

// Estructura que representa los datos del mapa IDJ en la EEPROM
typedef struct {
    uint16_t numero_jaula;       // 1–250, 0 = sin asignar
    char     unidad[12];         // "T0603-xxxx\0"
    uint32_t timestamp;          // Unix time de última asignación
    bool     valido;             // true si magic y CRC son correctos
} ds2431_data_t;

// Escritura y lectura de bajo nivel
esp_err_t ds2431_write_scratchpad(ds2482_t *ds2482, ds2431_t *dev,
                                   uint16_t addr, const uint8_t *data, size_t len);
esp_err_t ds2431_read_scratchpad(ds2482_t *ds2482, ds2431_t *dev,
                                  uint16_t *addr_es, uint8_t *data, size_t len);
esp_err_t ds2431_copy_scratchpad(ds2482_t *ds2482, ds2431_t *dev,
                                  uint16_t addr, uint8_t es_byte);
esp_err_t ds2431_read_memory(ds2482_t *ds2482, ds2431_t *dev,
                              uint16_t addr, uint8_t *data, size_t len);

// API de alto nivel IDJ
esp_err_t ds2431_escribir_datos(ds2482_t *ds2482, ds2431_t *dev,
                                 const ds2431_data_t *datos);
esp_err_t ds2431_leer_datos(ds2482_t *ds2482, ds2431_t *dev,
                             ds2431_data_t *datos);

// Utilidades
uint16_t  ds2431_crc16(const uint8_t *data, size_t len);
esp_err_t ds2431_match_rom(ds2482_t *ds2482, ds2431_t *dev);