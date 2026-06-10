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

// ── Mapa de EEPROM IDJ — Solo Jaulas (24 bytes = 3 bloques de 8) ──────────────
//
//  Block 0  (0x00–0x07)
//    0x00–0x01  Magic bytes   'I','D'  (0x49, 0x44)
//    0x02–0x03  numero_jaula  uint16_t little-endian
//    0x04–0x07  unidad[0..3]  chars de "T0603-"
//
//  Block 1  (0x08–0x0F)
//    0x08–0x0F  unidad[4..11] chars "xxxx\0\0\0"
//
//  Block 2  (0x10–0x17)
//    0x10–0x13  timestamp     uint32_t unix time
//    0x14–0x15  CRC-16        sobre bytes 0x00–0x13 (20 bytes)
//    0x16–0x17  padding       0x00
// ─────────────────────────────────────────────────────────────────────────────

#define DS2431_ADDR_MAGIC       0x00
#define DS2431_ADDR_JAULA       0x02
#define DS2431_ADDR_UNIDAD      0x04  // 12 bytes: "T0603-xxxx\0"
#define DS2431_ADDR_TIMESTAMP   0x10
#define DS2431_ADDR_CRC16       0x14
#define DS2431_CRC_DATA_LEN     20    // Bytes cubiertos por el CRC
#define DS2431_EEPROM_DATA_LEN  22    // Bytes de datos útiles (sin padding)
#define DS2431_EEPROM_BUF_LEN   24    // Buffer total = 3 × 8 bytes

#define DS2431_MAGIC_BYTE0      0x49  // 'I'
#define DS2431_MAGIC_BYTE1      0x44  // 'D'

#define DS2431_COPY_SCRATCHPAD_DELAY_MS  10

typedef struct {
    uint64_t rom_code;
} ds2431_t;

typedef struct {
    uint16_t numero_jaula;   // 1–9999, 0 = sin asignar
    char     unidad[12];     // "T0603-xxxx\0"
    uint32_t timestamp;      // Unix time de la última programación
    bool     valido;         // true si magic y CRC son correctos
} ds2431_data_t;

// API de bajo nivel
esp_err_t ds2431_match_rom(ds2482_t *ds2482, ds2431_t *dev);
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
