#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "ds2431.h"

#define TAG "DS2431"

// ─────────────────────────────────────────────────────────────────────────────
// CRC-16 (polinomio 0x8005, usado por DS2431)
// ─────────────────────────────────────────────────────────────────────────────
uint16_t ds2431_crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0x0000;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x8005 : (crc << 1);
        }
    }
    return crc;
}

// ─────────────────────────────────────────────────────────────────────────────
// Match ROM — selecciona un esclavo específico en el bus 1-Wire
// ─────────────────────────────────────────────────────────────────────────────
esp_err_t ds2431_match_rom(ds2482_t *ds2482, ds2431_t *dev) {
    esp_err_t err;
    bool presence = false;

    for (int intento = 0; intento < 3; intento++) {
        err = ds2482_1wire_reset(&presence);
        if (err == ESP_OK && presence) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (err != ESP_OK || !presence) {
        ESP_LOGE(TAG, "No hay presencia 1-Wire en match ROM");
        ds2482_reset(ds2482);
        ds2482_configure(ds2482, DS2482_CFG_APU);
        return ESP_ERR_NOT_FOUND;
    }

    err = ds2482_write_byte(OW_CMD_MATCH_ROM);
    if (err != ESP_OK) return err;

    uint8_t *rom_bytes = (uint8_t *)&dev->rom_code;
    for (int i = 0; i < 8; i++) {
        err = ds2482_write_byte(rom_bytes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error enviando ROM byte %d", i);
            return err;
        }
    }
    return ESP_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// Write Scratchpad — escribe hasta 8 bytes en dirección alineada
// ─────────────────────────────────────────────────────────────────────────────
esp_err_t ds2431_write_scratchpad(ds2482_t *ds2482, ds2431_t *dev,
                                   uint16_t addr, const uint8_t *data, size_t len) {
    if (len == 0 || len > 8) {
        ESP_LOGE(TAG, "Write Scratchpad: len debe ser 1–8 bytes, recibido %d", len);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ds2431_match_rom(ds2482, dev);
    if (err != ESP_OK) return err;

    err = ds2482_write_byte(DS2431_CMD_WRITE_SCRATCHPAD);
    if (err != ESP_OK) return err;

    err = ds2482_write_byte((uint8_t)(addr & 0xFF));
    if (err != ESP_OK) return err;
    err = ds2482_write_byte((uint8_t)(addr >> 8));
    if (err != ESP_OK) return err;

    for (size_t i = 0; i < len; i++) {
        err = ds2482_write_byte(data[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error escribiendo byte %d en scratchpad", i);
            return err;
        }
    }

    ESP_LOGD(TAG, "Write Scratchpad OK @ 0x%02X, %d bytes", addr, len);
    return ESP_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// Read Scratchpad — verifica antes del commit
// ─────────────────────────────────────────────────────────────────────────────
esp_err_t ds2431_read_scratchpad(ds2482_t *ds2482, ds2431_t *dev,
                                  uint16_t *addr_es, uint8_t *data, size_t len) {
    esp_err_t err = ds2431_match_rom(ds2482, dev);
    if (err != ESP_OK) return err;

    err = ds2482_write_byte(DS2431_CMD_READ_SCRATCHPAD);
    if (err != ESP_OK) return err;

    uint8_t ta1, ta2, es;
    err = ds2482_read_byte(&ta1); if (err != ESP_OK) return err;
    err = ds2482_read_byte(&ta2); if (err != ESP_OK) return err;
    err = ds2482_read_byte(&es);  if (err != ESP_OK) return err;

    if (addr_es) *addr_es = (uint16_t)ta1 | ((uint16_t)ta2 << 8);

    for (size_t i = 0; i < len; i++) {
        err = ds2482_read_byte(&data[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error leyendo scratchpad byte %d", i);
            return err;
        }
    }

    ESP_LOGD(TAG, "Read Scratchpad OK: TA1=0x%02X TA2=0x%02X ES=0x%02X", ta1, ta2, es);
    return ESP_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// Copy Scratchpad — commit a EEPROM (espera pasiva 15ms)
// ─────────────────────────────────────────────────────────────────────────────
esp_err_t ds2431_copy_scratchpad(ds2482_t *ds2482, ds2431_t *dev,
                                  uint16_t addr, uint8_t es_byte) {
    esp_err_t err = ds2431_match_rom(ds2482, dev);
    if (err != ESP_OK) return err;

    err = ds2482_write_byte(DS2431_CMD_COPY_SCRATCHPAD);
    if (err != ESP_OK) return err;

    err = ds2482_write_byte((uint8_t)(addr & 0xFF)); if (err != ESP_OK) return err;
    err = ds2482_write_byte((uint8_t)(addr >> 8));   if (err != ESP_OK) return err;
    err = ds2482_write_byte(es_byte);                if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "Copy Scratchpad addr=0x%02X es=0x%02X — grabando...", addr, es_byte);
    vTaskDelay(pdMS_TO_TICKS(15));  // DS2431 tarda máx 10ms; 15ms por margen

    return ESP_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// Read Memory — lectura directa de EEPROM
// ─────────────────────────────────────────────────────────────────────────────
esp_err_t ds2431_read_memory(ds2482_t *ds2482, ds2431_t *dev,
                              uint16_t addr, uint8_t *data, size_t len) {
    esp_err_t err = ds2431_match_rom(ds2482, dev);
    if (err != ESP_OK) return err;

    err = ds2482_write_byte(DS2431_CMD_READ_MEMORY);
    if (err != ESP_OK) return err;

    err = ds2482_write_byte((uint8_t)(addr & 0xFF)); if (err != ESP_OK) return err;
    err = ds2482_write_byte((uint8_t)(addr >> 8));   if (err != ESP_OK) return err;

    for (size_t i = 0; i < len; i++) {
        err = ds2482_read_byte(&data[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error leyendo memoria byte %d", i);
            return err;
        }
    }
    return ESP_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// API alto nivel: escribir datos IDJ v2 (jaula + dolly opcional)
//
// Layout del buffer (40 bytes = 5 bloques de 8):
//   Block 0  0x00–0x07  Magic + numero_jaula + unidad_jaula[0..3]
//   Block 1  0x08–0x0F  unidad_jaula[4..11]
//   Block 2  0x10–0x17  numero_dolly + unidad_dolly[0..5]
//   Block 3  0x18–0x1F  unidad_dolly[6..11] + padding
//   Block 4  0x20–0x27  timestamp + CRC-16 + padding
// ─────────────────────────────────────────────────────────────────────────────
esp_err_t ds2431_escribir_datos(ds2482_t *ds2482, ds2431_t *dev,
                                 const ds2431_data_t *datos) {
    uint8_t buf[DS2431_EEPROM_BUF_LEN];
    memset(buf, 0x00, sizeof(buf));

    // ── Magic ──────────────────────────────────────────────────────────────────
    buf[DS2431_ADDR_MAGIC]     = DS2431_MAGIC_BYTE0;
    buf[DS2431_ADDR_MAGIC + 1] = DS2431_MAGIC_BYTE1;

    // ── Jaula ──────────────────────────────────────────────────────────────────
    buf[DS2431_ADDR_JAULA]     = (uint8_t)(datos->numero_jaula & 0xFF);
    buf[DS2431_ADDR_JAULA + 1] = (uint8_t)(datos->numero_jaula >> 8);
    memcpy(&buf[DS2431_ADDR_UNIDAD_JAULA], datos->unidad_jaula, 12);

    // ── Dolly (0 = sin dolly, unidad queda en 0x00) ───────────────────────────
    buf[DS2431_ADDR_DOLLY]     = (uint8_t)(datos->numero_dolly & 0xFF);
    buf[DS2431_ADDR_DOLLY + 1] = (uint8_t)(datos->numero_dolly >> 8);
    if (datos->tiene_dolly && datos->numero_dolly > 0) {
        memcpy(&buf[DS2431_ADDR_UNIDAD_DOLLY], datos->unidad_dolly, 12);
    }

    // ── Timestamp ─────────────────────────────────────────────────────────────
    buf[DS2431_ADDR_TIMESTAMP]     = (uint8_t)(datos->timestamp & 0xFF);
    buf[DS2431_ADDR_TIMESTAMP + 1] = (uint8_t)((datos->timestamp >> 8)  & 0xFF);
    buf[DS2431_ADDR_TIMESTAMP + 2] = (uint8_t)((datos->timestamp >> 16) & 0xFF);
    buf[DS2431_ADDR_TIMESTAMP + 3] = (uint8_t)((datos->timestamp >> 24) & 0xFF);

    // ── CRC-16 sobre los primeros 36 bytes (0x00–0x23) ────────────────────────
    uint16_t crc = ds2431_crc16(buf, DS2431_CRC_DATA_LEN);
    buf[DS2431_ADDR_CRC16]     = (uint8_t)(crc & 0xFF);
    buf[DS2431_ADDR_CRC16 + 1] = (uint8_t)(crc >> 8);

    // ── Escribir en 5 bloques de 8 bytes ─────────────────────────────────────
    size_t pos = 0;
    while (pos < DS2431_EEPROM_BUF_LEN) {
        uint16_t addr = (uint16_t)pos;

        // Reset del bridge antes de cada bloque
        ds2482_1wire_reset(&(bool){false});
        vTaskDelay(pdMS_TO_TICKS(5));

        // 1. Escribir scratchpad
        esp_err_t err = ds2431_write_scratchpad(ds2482, dev, addr, &buf[pos], 8);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error write_scratchpad bloque 0x%02X", addr);
            return err;
        }

        // 2. Leer y verificar scratchpad
        uint8_t ta1, ta2, es_byte;
        uint8_t verify[8];
        ds2431_match_rom(ds2482, dev);
        ds2482_write_byte(DS2431_CMD_READ_SCRATCHPAD);
        ds2482_read_byte(&ta1);
        ds2482_read_byte(&ta2);
        ds2482_read_byte(&es_byte);
        for (int i = 0; i < 8; i++) ds2482_read_byte(&verify[i]);

        if (memcmp(&buf[pos], verify, 8) != 0) {
            ESP_LOGE(TAG, "Validación scratchpad falló en 0x%02X", addr);
            return ESP_ERR_INVALID_RESPONSE;
        }

        vTaskDelay(pdMS_TO_TICKS(10));

        // 3. Commit a EEPROM
        err = ds2431_copy_scratchpad(ds2482, dev, addr, es_byte);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error copy_scratchpad bloque 0x%02X", addr);
            return err;
        }

        ESP_LOGI(TAG, "Bloque 0x%02X grabado OK", addr);
        pos += 8;
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    return ESP_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// API alto nivel: leer y validar datos IDJ v2
//
// Detecta automáticamente si la EEPROM tiene formato antiguo (v1, CRC en 0x14)
// y lo informa, indicando que requiere reprogramación.
// ─────────────────────────────────────────────────────────────────────────────
esp_err_t ds2431_leer_datos(ds2482_t *ds2482, ds2431_t *dev,
                             ds2431_data_t *datos) {
    uint8_t buf[DS2431_EEPROM_BUF_LEN];
    memset(datos, 0, sizeof(ds2431_data_t));

    esp_err_t err = ds2431_read_memory(ds2482, dev, 0x00, buf, DS2431_EEPROM_BUF_LEN);
    if (err != ESP_OK) return err;

    // ── Verificar magic ───────────────────────────────────────────────────────
    if (buf[0] != DS2431_MAGIC_BYTE0 || buf[1] != DS2431_MAGIC_BYTE1) {
        ESP_LOGW(TAG, "EEPROM virgen (magic=0x%02X%02X)", buf[0], buf[1]);
        datos->valido = false;
        return ESP_OK;
    }

    // ── Verificar CRC v2 (36 bytes, CRC en 0x24) ──────────────────────────────
    uint16_t crc_leido     = (uint16_t)buf[DS2431_ADDR_CRC16]
                           | ((uint16_t)buf[DS2431_ADDR_CRC16 + 1] << 8);
    uint16_t crc_calculado = ds2431_crc16(buf, DS2431_CRC_DATA_LEN);

    if (crc_leido != crc_calculado) {
        // Intentar detectar formato antiguo v1 (CRC sobre 20 bytes en 0x14–0x15)
        uint16_t crc_v1_leido     = (uint16_t)buf[0x14] | ((uint16_t)buf[0x15] << 8);
        uint16_t crc_v1_calculado = ds2431_crc16(buf, 20);

        if (crc_v1_leido == crc_v1_calculado) {
            ESP_LOGW(TAG, "EEPROM con formato ANTIGUO (v1) — requiere reprogramación con firmware v2");
        } else {
            ESP_LOGE(TAG, "CRC inválido: leído=0x%04X calculado=0x%04X — datos corruptos",
                     crc_leido, crc_calculado);
        }
        datos->valido = false;
        return ESP_ERR_INVALID_CRC;
    }

    // ── Parsear jaula ─────────────────────────────────────────────────────────
    datos->numero_jaula = (uint16_t)buf[DS2431_ADDR_JAULA]
                        | ((uint16_t)buf[DS2431_ADDR_JAULA + 1] << 8);
    memcpy(datos->unidad_jaula, &buf[DS2431_ADDR_UNIDAD_JAULA], 12);
    datos->unidad_jaula[11] = '\0';

    // ── Parsear dolly ─────────────────────────────────────────────────────────
    datos->numero_dolly = (uint16_t)buf[DS2431_ADDR_DOLLY]
                        | ((uint16_t)buf[DS2431_ADDR_DOLLY + 1] << 8);
    datos->tiene_dolly  = (datos->numero_dolly > 0);
    if (datos->tiene_dolly) {
        memcpy(datos->unidad_dolly, &buf[DS2431_ADDR_UNIDAD_DOLLY], 12);
        datos->unidad_dolly[11] = '\0';
    }

    // ── Parsear timestamp ─────────────────────────────────────────────────────
    datos->timestamp = (uint32_t)buf[DS2431_ADDR_TIMESTAMP]
                     | ((uint32_t)buf[DS2431_ADDR_TIMESTAMP + 1] << 8)
                     | ((uint32_t)buf[DS2431_ADDR_TIMESTAMP + 2] << 16)
                     | ((uint32_t)buf[DS2431_ADDR_TIMESTAMP + 3] << 24);

    datos->valido = true;

    ESP_LOGI(TAG, "EEPROM OK — Jaula #%d '%s' | Dolly %s",
             datos->numero_jaula, datos->unidad_jaula,
             datos->tiene_dolly ? datos->unidad_dolly : "N/A");

    return ESP_OK;
}
