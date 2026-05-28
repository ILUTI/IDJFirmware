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

    // Reintentar el reset hasta 3 veces — el DS2431 puede estar
    // ocupado grabando cuando llega el reset entre bloques
    for (int intento = 0; intento < 3; intento++) {
        err = ds2482_1wire_reset(&presence);
        if (err == ESP_OK && presence) break;
        vTaskDelay(pdMS_TO_TICKS(30));  // 30ms: bus de 80m necesita más tiempo entre reintentos
    }

    if (err != ESP_OK || !presence) {
        ESP_LOGE(TAG, "No hay presencia 1-Wire en match ROM");
        ds2482_reset(ds2482); 
        ds2482_configure(ds2482, DS2482_CFG_APU); // Re-aplicar configuración
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

    // 1. Seleccionamos el esclavo
    err = ds2431_match_rom(ds2482, dev);
    if (err != ESP_OK) return err;
    // 2. Enviamos comando Copy
    err = ds2482_write_byte(DS2431_CMD_COPY_SCRATCHPAD);
    if (err != ESP_OK) return err;

    // 3. Enviamos los bytes de validación
    err = ds2482_write_byte((uint8_t)(addr & 0xFF)); if (err != ESP_OK) return err;
    err = ds2482_write_byte((uint8_t)(addr >> 8));   if (err != ESP_OK) return err;
    err = ds2482_write_byte(es_byte);                if (err != ESP_OK) return err;

    // Log para diagnóstico — ver qué bytes llegan durante el polling
    ESP_LOGI(TAG, "Copy Scratchpad addr=0x%02X es=0x%02X — iniciando polling", addr, es_byte);

// 4. ESPERA PASIVA (Punto 3): 
    // En lugar de hacer polling en el bus, dejamos que el chip trabaje en silencio.
    // El DS2431 tarda 10ms máximo; usamos 15ms por seguridad en ambiente ruidoso.
    vTaskDelay(pdMS_TO_TICKS(15)); 


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
esp_err_t ds2431_escribir_datos(ds2482_t *ds2482, ds2431_t *dev, const ds2431_data_t *datos) {
    // 1. AUMENTAR EL BUFFER A 24 BYTES (Múltiplo de 8)
    uint8_t buf[24]; 
    memset(buf, 0x00, sizeof(buf)); // Todo inicializado en ceros

    // --- Preparación de datos ---
    buf[0] = DS2431_MAGIC_BYTE0; 
    buf[1] = DS2431_MAGIC_BYTE1;
    buf[2] = (uint8_t)(datos->numero_jaula & 0xFF);
    buf[3] = (uint8_t)(datos->numero_jaula >> 8);
    strncpy((char *)&buf[4], datos->unidad, 11);
    
    // Timestamp (dirección 0x10 a 0x13)
    buf[16] = (uint8_t)(datos->timestamp & 0xFF);
    buf[17] = (uint8_t)((datos->timestamp >> 8) & 0xFF);
    buf[18] = (uint8_t)((datos->timestamp >> 16) & 0xFF);
    buf[19] = (uint8_t)((datos->timestamp >> 24) & 0xFF);
    
    // Calcular CRC de los primeros 20 bytes
    uint16_t crc = ds2431_crc16(buf, 20);
    buf[20] = (uint8_t)(crc & 0xFF);
    buf[21] = (uint8_t)(crc >> 8);
    
    // buf[22] y buf[23] se quedan en 0x00 como relleno (Padding)

    // 2. FIJAR EL TOTAL EN 24 BYTES
    size_t total = 24; 
    size_t pos = 0;

    while (pos < total) {
        // 3. EL TAMAÑO DE BLOQUE AHORA SIEMPRE ES 8
        size_t tam_bloque = 8; 
        uint16_t addr = (uint16_t)pos;

        // Reset de bridge antes de cada bloque — 15ms para bus de 80m
        ds2482_1wire_reset(&(bool){false});
        vTaskDelay(pdMS_TO_TICKS(15));

        // Escribir Scratchpad
        esp_err_t err = ds2431_write_scratchpad(ds2482, dev, addr, &buf[pos], tam_bloque);
        if (err != ESP_OK) return err;

        // Leer Scratchpad para verificar antes de hacer el copy
        uint8_t ta1, ta2, es_byte;
        uint8_t verify[8];
        err = ds2431_match_rom(ds2482, dev);
        if (err != ESP_OK) return err;
        err = ds2482_write_byte(DS2431_CMD_READ_SCRATCHPAD);
        if (err != ESP_OK) return err;
        err = ds2482_read_byte(&ta1);  if (err != ESP_OK) return err;
        err = ds2482_read_byte(&ta2);  if (err != ESP_OK) return err;
        err = ds2482_read_byte(&es_byte); if (err != ESP_OK) return err;
        for (int i = 0; i < (int)tam_bloque; i++) {
            err = ds2482_read_byte(&verify[i]);
            if (err != ESP_OK) return err;
        }

        if (memcmp(&buf[pos], verify, tam_bloque) != 0) {
            ESP_LOGE(TAG, "Error de validacion scratchpad en 0x%02X", addr);
            return ESP_ERR_INVALID_RESPONSE;
        }

        vTaskDelay(pdMS_TO_TICKS(20));  // 20ms: bus de 80m necesita más margen antes del copy

        // Copiar a EEPROM
        err = ds2431_copy_scratchpad(ds2482, dev, addr, es_byte);
        if (err != ESP_OK) return err;

        ESP_LOGI(TAG, "Bloque 0x%02X grabado OK", addr);

        pos += tam_bloque;
        vTaskDelay(pdMS_TO_TICKS(40)); // 40ms entre bloques: enfriamiento EEPROM + bus recovery
    }
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