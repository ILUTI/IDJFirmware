#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "ds2431.h"

#define TAG "DS2431"

// ─────────────────────────────────────────────
// CRC-16 (polinomio 0x8005, usado por DS2431)
// ─────────────────────────────────────────────
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

// ─────────────────────────────────────────────
// Match ROM: selecciona un esclavo específico
// en el bus 1-Wire por su ROM de 64 bits.
// Debe llamarse justo después de un reset 1-Wire.
// ─────────────────────────────────────────────
esp_err_t ds2431_match_rom(ds2482_t *ds2482, ds2431_t *dev) {
    esp_err_t err;
    bool presence = false;

    err = ds2482_1wire_reset(&presence);
    if (err != ESP_OK || !presence) {
        ESP_LOGE(TAG, "No hay presencia 1-Wire en match ROM");
        return ESP_ERR_NOT_FOUND;
    }

    // Enviar comando Match ROM
    err = ds2482_write_byte(OW_CMD_MATCH_ROM);
    if (err != ESP_OK) return err;

    // Enviar los 8 bytes del ROM code LSB primero
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

// ─────────────────────────────────────────────
// Write Scratchpad
// El DS2431 exige escribir en bloques de 8 bytes
// alineados. addr debe ser múltiplo de 8.
// El DS2482 devuelve el byte E/S (ES) que necesita
// Copy Scratchpad para confirmar la escritura.
// ─────────────────────────────────────────────
esp_err_t ds2431_write_scratchpad(ds2482_t *ds2482, ds2431_t *dev,
                                   uint16_t addr, const uint8_t *data, size_t len) {
    esp_err_t err;

    if (len == 0 || len > 8) {
        ESP_LOGE(TAG, "Write Scratchpad: len debe ser 1–8 bytes, recibido %d", len);
        return ESP_ERR_INVALID_ARG;
    }

    err = ds2431_match_rom(ds2482, dev);
    if (err != ESP_OK) return err;

    // Comando Write Scratchpad
    err = ds2482_write_byte(DS2431_CMD_WRITE_SCRATCHPAD);
    if (err != ESP_OK) return err;

    // Dirección destino (TA1 = LSB, TA2 = MSB)
    err = ds2482_write_byte((uint8_t)(addr & 0xFF));
    if (err != ESP_OK) return err;
    err = ds2482_write_byte((uint8_t)(addr >> 8));
    if (err != ESP_OK) return err;

    // Datos
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

// ─────────────────────────────────────────────
// Read Scratchpad
// Verifica que lo que se escribió en el scratchpad
// coincide antes de hacer el Copy (commit).
// Devuelve addr_es = dirección + byte ES para
// pasarle a Copy Scratchpad.
// ─────────────────────────────────────────────
esp_err_t ds2431_read_scratchpad(ds2482_t *ds2482, ds2431_t *dev,
                                  uint16_t *addr_es, uint8_t *data, size_t len) {
    esp_err_t err;

    err = ds2431_match_rom(ds2482, dev);
    if (err != ESP_OK) return err;

    err = ds2482_write_byte(DS2431_CMD_READ_SCRATCHPAD);
    if (err != ESP_OK) return err;

    // Leer TA1, TA2 (dirección) y ES (byte de estado/offset)
    uint8_t ta1, ta2, es;
    err = ds2482_read_byte(&ta1); if (err != ESP_OK) return err;
    err = ds2482_read_byte(&ta2); if (err != ESP_OK) return err;
    err = ds2482_read_byte(&es);  if (err != ESP_OK) return err;

    if (addr_es) {
        *addr_es = (uint16_t)ta1 | ((uint16_t)ta2 << 8);
    }

    // Leer los datos del scratchpad
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

// ─────────────────────────────────────────────
// Copy Scratchpad — hace el commit a EEPROM
// CRÍTICO: addr y es_byte deben coincidir
// exactamente con lo que devolvió Read Scratchpad.
// El DS2431 necesita ~10ms para grabar.
// ─────────────────────────────────────────────
esp_err_t ds2431_copy_scratchpad(ds2482_t *ds2482, ds2431_t *dev,
                                  uint16_t addr, uint8_t es_byte) {
    esp_err_t err;

    err = ds2431_match_rom(ds2482, dev);
    if (err != ESP_OK) return err;

    err = ds2482_write_byte(DS2431_CMD_COPY_SCRATCHPAD);
    if (err != ESP_OK) return err;

    // Enviar TA1, TA2, ES — deben ser idénticos a los del Write Scratchpad
    err = ds2482_write_byte((uint8_t)(addr & 0xFF)); if (err != ESP_OK) return err;
    err = ds2482_write_byte((uint8_t)(addr >> 8));   if (err != ESP_OK) return err;
    err = ds2482_write_byte(es_byte);                if (err != ESP_OK) return err;

    // Esperar tiempo de grabación EEPROM (datasheet: máx 10ms)
    vTaskDelay(pdMS_TO_TICKS(DS2431_COPY_SCRATCHPAD_DELAY_MS));

    ESP_LOGD(TAG, "Copy Scratchpad OK @ 0x%02X", addr);
    return ESP_OK;
}

// ─────────────────────────────────────────────
// Read Memory — lectura directa de EEPROM
// ─────────────────────────────────────────────
esp_err_t ds2431_read_memory(ds2482_t *ds2482, ds2431_t *dev,
                              uint16_t addr, uint8_t *data, size_t len) {
    esp_err_t err;

    err = ds2431_match_rom(ds2482, dev);
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

// ─────────────────────────────────────────────
// API alto nivel: escribir datos IDJ en EEPROM
// Escribe en bloques de 8 bytes (requerimiento DS2431)
// con verificación de scratchpad antes de cada commit.
// ─────────────────────────────────────────────
esp_err_t ds2431_escribir_datos(ds2482_t *ds2482, ds2431_t *dev,
                                 const ds2431_data_t *datos) {
    // Construir el buffer completo de 22 bytes según el mapa IDJ
    uint8_t buf[DS2431_EEPROM_DATA_LEN];
    memset(buf, 0x00, sizeof(buf));

    // Magic number
    buf[0] = DS2431_MAGIC_BYTE0;  // 'I'
    buf[1] = DS2431_MAGIC_BYTE1;  // 'D'

    // Número de jaula (little-endian)
    buf[2] = (uint8_t)(datos->numero_jaula & 0xFF);
    buf[3] = (uint8_t)(datos->numero_jaula >> 8);

    // Código de unidad (12 bytes, null-padded)
    strncpy((char *)&buf[4], datos->unidad, 11);
    buf[15] = 0x00;  // garantizar null-terminator

    // Timestamp (little-endian)
    buf[16] = (uint8_t)(datos->timestamp & 0xFF);
    buf[17] = (uint8_t)((datos->timestamp >> 8)  & 0xFF);
    buf[18] = (uint8_t)((datos->timestamp >> 16) & 0xFF);
    buf[19] = (uint8_t)((datos->timestamp >> 24) & 0xFF);

    // CRC-16 de bytes 0x00–0x13 (primeros 20 bytes)
    uint16_t crc = ds2431_crc16(buf, 20);
    buf[20] = (uint8_t)(crc & 0xFF);
    buf[21] = (uint8_t)(crc >> 8);

    ESP_LOGI(TAG, "Escribiendo jaula #%d unidad '%s' timestamp %lu CRC 0x%04X",
             datos->numero_jaula, datos->unidad, datos->timestamp, crc);

    // Escribir en bloques de 8 bytes (páginas del DS2431)
    // El DS2431 tiene páginas de 8 bytes — cada Write+Copy es una página
    esp_err_t err;
    size_t total = sizeof(buf);
    size_t escrito = 0;

    while (escrito < total) {
        size_t bloque = (total - escrito > 8) ? 8 : (total - escrito);
        uint16_t addr = (uint16_t)escrito;

        // 1. Escribir al scratchpad
        err = ds2431_write_scratchpad(ds2482, dev, addr, &buf[escrito], bloque);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Fallo write scratchpad en bloque addr=0x%02X", addr);
            return err;
        }

        // 2. Leer scratchpad de vuelta para verificar
        uint8_t verify[8];
        uint16_t addr_readback;
        err = ds2431_read_scratchpad(ds2482, dev, &addr_readback, verify, bloque);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Fallo read scratchpad en bloque addr=0x%02X", addr);
            return err;
        }

        if (memcmp(&buf[escrito], verify, bloque) != 0) {
            ESP_LOGE(TAG, "Verificación scratchpad falló en addr=0x%02X", addr);
            return ESP_ERR_INVALID_RESPONSE;
        }

        // 3. Commit a EEPROM — ES byte = (addr + bloque - 1) & 0x07
        uint8_t es_byte = (uint8_t)((addr + bloque - 1) & 0x07);
        err = ds2431_copy_scratchpad(ds2482, dev, addr, es_byte);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Fallo copy scratchpad en bloque addr=0x%02X", addr);
            return err;
        }

        escrito += bloque;
        ESP_LOGD(TAG, "Bloque 0x%02X–0x%02X grabado OK", addr, addr + bloque - 1);
    }

    ESP_LOGI(TAG, "Escritura EEPROM completa — %d bytes en %d bloques",
             total, (total + 7) / 8);
    return ESP_OK;
}

// ─────────────────────────────────────────────
// API alto nivel: leer y validar datos IDJ
// ─────────────────────────────────────────────
esp_err_t ds2431_leer_datos(ds2482_t *ds2482, ds2431_t *dev,
                             ds2431_data_t *datos) {
    uint8_t buf[DS2431_EEPROM_DATA_LEN];
    memset(datos, 0, sizeof(ds2431_data_t));

    esp_err_t err = ds2431_read_memory(ds2482, dev, 0x00, buf, sizeof(buf));
    if (err != ESP_OK) return err;

    // Verificar magic number
    if (buf[0] != DS2431_MAGIC_BYTE0 || buf[1] != DS2431_MAGIC_BYTE1) {
        ESP_LOGW(TAG, "EEPROM virgen o no programada (magic=0x%02X%02X)", buf[0], buf[1]);
        datos->valido = false;
        return ESP_OK;  // No es error, simplemente no está asignada
    }

    // Verificar CRC-16
    uint16_t crc_leido    = (uint16_t)buf[20] | ((uint16_t)buf[21] << 8);
    uint16_t crc_calculado = ds2431_crc16(buf, 20);

    if (crc_leido != crc_calculado) {
        ESP_LOGE(TAG, "CRC inválido: leído=0x%04X calculado=0x%04X — datos corruptos",
                 crc_leido, crc_calculado);
        datos->valido = false;
        return ESP_ERR_INVALID_CRC;
    }

    // Parsear datos
    datos->numero_jaula = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
    memcpy(datos->unidad, &buf[4], 12);
    datos->unidad[11]   = '\0';
    datos->timestamp    = (uint32_t)buf[16]
                        | ((uint32_t)buf[17] << 8)
                        | ((uint32_t)buf[18] << 16)
                        | ((uint32_t)buf[19] << 24);
    datos->valido       = true;

    ESP_LOGI(TAG, "EEPROM OK — Jaula #%d unidad '%s' timestamp %lu",
             datos->numero_jaula, datos->unidad, datos->timestamp);

    return ESP_OK;
}