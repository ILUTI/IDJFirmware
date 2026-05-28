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

// ── Mapa de EEPROM IDJ v2 (con soporte Dolly) ─────────────────────────────────
//
//  Block 0  (0x00–0x07)
//    0x00–0x01  Magic bytes     'I','D'  (0x49, 0x44)
//    0x02–0x03  numero_jaula    uint16_t little-endian (1-9999, 0 = sin asignar)
//    0x04–0x07  unidad_jaula    chars 0..3  ("T060")
//
//  Block 1  (0x08–0x0F)
//    0x08–0x0F  unidad_jaula    chars 4..11 ("3-xxxx\0\0")
//
//  Block 2  (0x10–0x17)
//    0x10–0x11  numero_dolly    uint16_t little-endian (0 = sin dolly)
//    0x12–0x17  unidad_dolly    chars 0..5  ("T0605-")
//
//  Block 3  (0x18–0x1F)
//    0x18–0x1D  unidad_dolly    chars 6..11 ("xxxx\0\0")
//    0x1E–0x1F  padding         0x00
//
//  Block 4  (0x20–0x27)
//    0x20–0x23  timestamp       uint32_t unix time
//    0x24–0x25  CRC-16          sobre bytes 0x00–0x23 (36 bytes)
//    0x26–0x27  padding         0x00
//
//  Total: 40 bytes = 5 bloques de 8 bytes
// ─────────────────────────────────────────────────────────────────────────────

#define DS2431_ADDR_MAGIC           0x00  // 2 bytes: magic 'I','D'
#define DS2431_ADDR_JAULA           0x02  // 2 bytes: numero_jaula uint16_t
#define DS2431_ADDR_UNIDAD_JAULA    0x04  // 12 bytes: "T0603-xxxx\0\0"
#define DS2431_ADDR_DOLLY           0x10  // 2 bytes: numero_dolly uint16_t (0 = sin dolly)
#define DS2431_ADDR_UNIDAD_DOLLY    0x12  // 12 bytes: "T0605-xxxx\0\0"
#define DS2431_ADDR_TIMESTAMP       0x20  // 4 bytes: unix time uint32_t
#define DS2431_ADDR_CRC16           0x24  // 2 bytes: CRC-16

#define DS2431_CRC_DATA_LEN         36    // Bytes cubiertos por el CRC (0x00–0x23)
#define DS2431_EEPROM_DATA_LEN      38    // Bytes útiles totales (sin padding final)
#define DS2431_EEPROM_BUF_LEN       40    // Buffer de trabajo = 5 × 8 bytes

#define DS2431_MAGIC_BYTE0          0x49  // 'I'
#define DS2431_MAGIC_BYTE1          0x44  // 'D'

// Tiempo de escritura EEPROM (máx 10ms por bloque según datasheet DS2431)
#define DS2431_COPY_SCRATCHPAD_DELAY_MS  10

typedef struct {
    uint64_t rom_code;
} ds2431_t;

// ── Estructura de datos IDJ v2 ────────────────────────────────────────────────
typedef struct {
    // Jaula
    uint16_t numero_jaula;       // 1–9999 (0 = sin asignar)
    char     unidad_jaula[12];   // "T0603-xxxx\0" (null terminado, 12 bytes)

    // Dolly (opcional)
    uint16_t numero_dolly;       // 1–9999 (0 = sin dolly)
    char     unidad_dolly[12];   // "T0605-xxxx\0" (vacío si numero_dolly == 0)
    bool     tiene_dolly;        // true si numero_dolly > 0

    // Metadata
    uint32_t timestamp;          // Unix time de la última programación
    bool     valido;             // true si magic y CRC son correctos
} ds2431_data_t;

// ── API de bajo nivel ─────────────────────────────────────────────────────────
esp_err_t ds2431_match_rom(ds2482_t *ds2482, ds2431_t *dev);
esp_err_t ds2431_write_scratchpad(ds2482_t *ds2482, ds2431_t *dev,
                                   uint16_t addr, const uint8_t *data, size_t len);
esp_err_t ds2431_read_scratchpad(ds2482_t *ds2482, ds2431_t *dev,
                                  uint16_t *addr_es, uint8_t *data, size_t len);
esp_err_t ds2431_copy_scratchpad(ds2482_t *ds2482, ds2431_t *dev,
                                  uint16_t addr, uint8_t es_byte);
esp_err_t ds2431_read_memory(ds2482_t *ds2482, ds2431_t *dev,
                              uint16_t addr, uint8_t *data, size_t len);

// ── API de alto nivel IDJ ─────────────────────────────────────────────────────
esp_err_t ds2431_escribir_datos(ds2482_t *ds2482, ds2431_t *dev,
                                 const ds2431_data_t *datos);
esp_err_t ds2431_leer_datos(ds2482_t *ds2482, ds2431_t *dev,
                             ds2431_data_t *datos);

// ── Utilidades ────────────────────────────────────────────────────────────────
uint16_t  ds2431_crc16(const uint8_t *data, size_t len);
