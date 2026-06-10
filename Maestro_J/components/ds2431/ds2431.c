#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "ds2431.h"

#define TAG "DS2431"

// ─────────────────────────────────────────────────────────────────────────────
// CRC-16 (polinomio 0x8005)
// ─────────────────────────────────────────────────────────────────────────────
uint16_t ds2431_crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0x0000;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x8005 : (crc << 1);
    }
    return crc;
}

// ─────────────────────────────────────────────────────────────────────────────
// Match ROM — 3 reintentos con 30ms entre ellos (bus largo ~80m)
// ─────────────────────────────────────────────────────────────────────────────
esp_err_t ds2431_match_rom(ds2482_t *ds2482, ds2431_t *dev) {
    esp_err_t err;
    bool presence = false;
    for (int i = 0; i < 3; i++) {
        err = ds2482_1wire_reset(&presence);
        if (err == ESP_OK && presence) break;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    if (err != ESP_OK || !presence) {
        ESP_LOGE(TAG, "No hay presencia 1-Wire");
        ds2482_reset(ds2482);
        ds2482_configure(ds2482, DS2482_CFG_APU);
        return ESP_ERR_NOT_FOUND;
    }
    err = ds2482_write_byte(OW_CMD_MATCH_ROM);
    if (err != ESP_OK) return err;
    uint8_t *rom = (uint8_t *)&dev->rom_code;
    for (int i = 0; i < 8; i++) {
        err = ds2482_write_byte(rom[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// Write Scratchpad
// ─────────────────────────────────────────────────────────────────────────────
esp_err_t ds2431_write_scratchpad(ds2482_t *ds2482, ds2431_t *dev,
                                   uint16_t addr, const uint8_t *data, size_t len) {
    if (len == 0 || len > 8) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ds2431_match_rom(ds2482, dev);
    if (err != ESP_OK) return err;
    err = ds2482_write_byte(DS2431_CMD_WRITE_SCRATCHPAD); if (err != ESP_OK) return err;
    err = ds2482_write_byte((uint8_t)(addr & 0xFF));      if (err != ESP_OK) return err;
    err = ds2482_write_byte((uint8_t)(addr >> 8));        if (err != ESP_OK) return err;
    for (size_t i = 0; i < len; i++) {
        err = ds2482_write_byte(data[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// Read Scratchpad
// ─────────────────────────────────────────────────────────────────────────────
esp_err_t ds2431_read_scratchpad(ds2482_t *ds2482, ds2431_t *dev,
                                  uint16_t *addr_es, uint8_t *data, size_t len) {
    esp_err_t err = ds2431_match_rom(ds2482, dev);
    if (err != ESP_OK) return err;
    err = ds2482_write_byte(DS2431_CMD_READ_SCRATCHPAD); if (err != ESP_OK) return err;
    uint8_t ta1, ta2, es;
    err = ds2482_read_byte(&ta1); if (err != ESP_OK) return err;
    err = ds2482_read_byte(&ta2); if (err != ESP_OK) return err;
    err = ds2482_read_byte(&es);  if (err != ESP_OK) return err;
    if (addr_es) *addr_es = (uint16_t)ta1 | ((uint16_t)ta2 << 8);
    for (size_t i = 0; i < len; i++) {
        err = ds2482_read_byte(&data[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// Copy Scratchpad — espera pasiva 15ms
// ─────────────────────────────────────────────────────────────────────────────
esp_err_t ds2431_copy_scratchpad(ds2482_t *ds2482, ds2431_t *dev,
                                  uint16_t addr, uint8_t es_byte) {
    esp_err_t err = ds2431_match_rom(ds2482, dev);
    if (err != ESP_OK) return err;
    err = ds2482_write_byte(DS2431_CMD_COPY_SCRATCHPAD); if (err != ESP_OK) return err;
    err = ds2482_write_byte((uint8_t)(addr & 0xFF));     if (err != ESP_OK) return err;
    err = ds2482_write_byte((uint8_t)(addr >> 8));       if (err != ESP_OK) return err;
    err = ds2482_write_byte(es_byte);                    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(15));
    return ESP_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// Read Memory
// ─────────────────────────────────────────────────────────────────────────────
esp_err_t ds2431_read_memory(ds2482_t *ds2482, ds2431_t *dev,
                              uint16_t addr, uint8_t *data, size_t len) {
    esp_err_t err = ds2431_match_rom(ds2482, dev);
    if (err != ESP_OK) return err;
    err = ds2482_write_byte(DS2431_CMD_READ_MEMORY);     if (err != ESP_OK) return err;
    err = ds2482_write_byte((uint8_t)(addr & 0xFF));     if (err != ESP_OK) return err;
    err = ds2482_write_byte((uint8_t)(addr >> 8));       if (err != ESP_OK) return err;
    for (size_t i = 0; i < len; i++) {
        err = ds2482_read_byte(&data[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// Escribir datos IDJ — solo jaula (24 bytes, 3 bloques de 8)
// ─────────────────────────────────────────────────────────────────────────────
esp_err_t ds2431_escribir_datos(ds2482_t *ds2482, ds2431_t *dev,
                                 const ds2431_data_t *datos) {
    uint8_t buf[DS2431_EEPROM_BUF_LEN];
    memset(buf, 0x00, sizeof(buf));

    buf[DS2431_ADDR_MAGIC]     = DS2431_MAGIC_BYTE0;
    buf[DS2431_ADDR_MAGIC + 1] = DS2431_MAGIC_BYTE1;
    buf[DS2431_ADDR_JAULA]     = (uint8_t)(datos->numero_jaula & 0xFF);
    buf[DS2431_ADDR_JAULA + 1] = (uint8_t)(datos->numero_jaula >> 8);
    memcpy(&buf[DS2431_ADDR_UNIDAD], datos->unidad, 12);
    buf[DS2431_ADDR_TIMESTAMP]     = (uint8_t)(datos->timestamp & 0xFF);
    buf[DS2431_ADDR_TIMESTAMP + 1] = (uint8_t)((datos->timestamp >> 8)  & 0xFF);
    buf[DS2431_ADDR_TIMESTAMP + 2] = (uint8_t)((datos->timestamp >> 16) & 0xFF);
    buf[DS2431_ADDR_TIMESTAMP + 3] = (uint8_t)((datos->timestamp >> 24) & 0xFF);

    uint16_t crc = ds2431_crc16(buf, DS2431_CRC_DATA_LEN);
    buf[DS2431_ADDR_CRC16]     = (uint8_t)(crc & 0xFF);
    buf[DS2431_ADDR_CRC16 + 1] = (uint8_t)(crc >> 8);

    // Escribir 3 bloques de 8 bytes
    size_t pos = 0;
    while (pos < DS2431_EEPROM_BUF_LEN) {
        uint16_t addr = (uint16_t)pos;

        ds2482_1wire_reset(&(bool){false});
        vTaskDelay(pdMS_TO_TICKS(15));

        esp_err_t err = ds2431_write_scratchpad(ds2482, dev, addr, &buf[pos], 8);
        if (err != ESP_OK) return err;

        // Verificar scratchpad
        uint8_t ta1, ta2, es_byte, verify[8];
        ds2431_match_rom(ds2482, dev);
        ds2482_write_byte(DS2431_CMD_READ_SCRATCHPAD);
        ds2482_read_byte(&ta1); ds2482_read_byte(&ta2); ds2482_read_byte(&es_byte);
        for (int i = 0; i < 8; i++) ds2482_read_byte(&verify[i]);

        if (memcmp(&buf[pos], verify, 8) != 0) {
            ESP_LOGE(TAG, "Validación scratchpad falló en 0x%02X", addr);
            return ESP_ERR_INVALID_RESPONSE;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
        err = ds2431_copy_scratchpad(ds2482, dev, addr, es_byte);
        if (err != ESP_OK) return err;

        ESP_LOGI(TAG, "Bloque 0x%02X grabado OK", addr);
        pos += 8;
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    return ESP_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// Leer y validar datos IDJ — solo jaula
// ─────────────────────────────────────────────────────────────────────────────
esp_err_t ds2431_leer_datos(ds2482_t *ds2482, ds2431_t *dev,
                             ds2431_data_t *datos) {
    uint8_t buf[DS2431_EEPROM_BUF_LEN];
    memset(datos, 0, sizeof(ds2431_data_t));

    esp_err_t err = ds2431_read_memory(ds2482, dev, 0x00, buf, DS2431_EEPROM_BUF_LEN);
    if (err != ESP_OK) return err;

    if (buf[0] != DS2431_MAGIC_BYTE0 || buf[1] != DS2431_MAGIC_BYTE1) {
        ESP_LOGW(TAG, "EEPROM virgen (magic=0x%02X%02X)", buf[0], buf[1]);
        datos->valido = false;
        return ESP_OK;
    }

    uint16_t crc_leido     = (uint16_t)buf[DS2431_ADDR_CRC16]
                           | ((uint16_t)buf[DS2431_ADDR_CRC16 + 1] << 8);
    uint16_t crc_calculado = ds2431_crc16(buf, DS2431_CRC_DATA_LEN);

    if (crc_leido != crc_calculado) {
        ESP_LOGE(TAG, "CRC inválido: leído=0x%04X calculado=0x%04X",
                 crc_leido, crc_calculado);
        datos->valido = false;
        return ESP_ERR_INVALID_CRC;
    }

    datos->numero_jaula = (uint16_t)buf[DS2431_ADDR_JAULA]
                        | ((uint16_t)buf[DS2431_ADDR_JAULA + 1] << 8);
    memcpy(datos->unidad, &buf[DS2431_ADDR_UNIDAD], 12);
    datos->unidad[11] = '\0';
    datos->timestamp  = (uint32_t)buf[DS2431_ADDR_TIMESTAMP]
                      | ((uint32_t)buf[DS2431_ADDR_TIMESTAMP + 1] << 8)
                      | ((uint32_t)buf[DS2431_ADDR_TIMESTAMP + 2] << 16)
                      | ((uint32_t)buf[DS2431_ADDR_TIMESTAMP + 3] << 24);
    datos->valido = true;

    ESP_LOGI(TAG, "EEPROM OK — Jaula #%d '%s'",
             datos->numero_jaula, datos->unidad);
    return ESP_OK;
}
