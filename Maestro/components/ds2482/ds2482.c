#include "ds2482.h"
#include "esp_log.h"
#include "string.h"

#define TAG "DS2482"

static i2c_port_t g_i2c_num;
static uint8_t g_i2c_addr;

#define DS2482_CMD_DEVICE_RESET   0xF0
#define DS2482_CMD_SET_READ_PTR   0xE1
#define DS2482_CMD_WRITE_BYTE     0xA5
#define DS2482_CMD_READ_BYTE      0x96
#define DS2482_CMD_1WIRE_RESET    0xB4
#define DS2482_CMD_1WIRE_SINGLE   0x87
#define DS2482_CMD_1WIRE_TRIPLET  0x78

#define DS2482_REG_STATUS         0xF0
#define DS2482_STATUS_BUSY        0x01
#define DS2482_STATUS_PPD         0x02
#define DS2482_STATUS_SD          0x04
#define DS2482_STATUS_LL          0x08
#define DS2482_STATUS_RST         0x10
#define DS2482_STATUS_SBR         0x20
#define DS2482_STATUS_TSB         0x40
#define DS2482_STATUS_DIR         0x80

esp_err_t ds2482_init(ds2482_t *dev, i2c_port_t i2c_num, uint8_t address) {
    dev->i2c_num = i2c_num;
    dev->address = address;
    g_i2c_num = i2c_num;    // <<-- agregar
    g_i2c_addr = address;   // <<-- agregar
    return ds2482_reset(dev);
}

esp_err_t ds2482_reset(ds2482_t *dev) {
    uint8_t cmd = DS2482_CMD_DEVICE_RESET;
    return i2c_master_write_to_device(dev->i2c_num, dev->address, &cmd, 1, pdMS_TO_TICKS(100));
}

esp_err_t ds2482_set_read_pointer(uint8_t reg) {
    uint8_t cmd[2] = { DS2482_CMD_SET_READ_PTR, reg };
    return i2c_master_write_to_device(g_i2c_num, g_i2c_addr, cmd, 2, pdMS_TO_TICKS(100));
}

esp_err_t ds2482_read_register(uint8_t *value) {
    return i2c_master_read_from_device(g_i2c_num, g_i2c_addr, value, 1, pdMS_TO_TICKS(100));
}

// Espera a que el DS2482 libere el bus 1-Wire.
// Retorna ESP_FAIL inmediatamente si I2C falla — no loopea con bus muerto.
esp_err_t ds2482_busy_wait() {
    int intentos = 0;
    while (true) {
        uint8_t status;
        esp_err_t err = ds2482_set_read_pointer(DS2482_REG_STATUS);
        if (err != ESP_OK) return err;
        err = ds2482_read_register(&status);
        if (err != ESP_OK) return err;
        if (!(status & DS2482_STATUS_BUSY)) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(3));  // 3ms entre polls: reduce ruido I2C en bus capacitivo
        if (++intentos >= 200) {
            ESP_LOGE(TAG, "busy_wait timeout — bus 1-Wire bloqueado");
            return ESP_ERR_TIMEOUT;
        }
    }
}

esp_err_t ds2482_1wire_reset(bool *presence) {
    uint8_t cmd = DS2482_CMD_1WIRE_RESET;
    esp_err_t err;
    err = ds2482_busy_wait();
    if (err != ESP_OK) return err;
    err = i2c_master_write_to_device(g_i2c_num, g_i2c_addr, &cmd, 1, pdMS_TO_TICKS(100));
    if (err != ESP_OK) return err;
    err = ds2482_busy_wait();
    if (err != ESP_OK) return err;

    uint8_t status;
    ds2482_set_read_pointer(DS2482_REG_STATUS);
    ds2482_read_register(&status);

    *presence = (status & DS2482_STATUS_PPD) != 0;
    // Pausa post-reset: 8ms para 80m de cable (~4-8nF de capacitancia).
    // El APU maneja el recovery del slot, pero el bus necesita estabilizarse
    // antes de que el master empiece a enviar Match ROM.
    vTaskDelay(pdMS_TO_TICKS(8));
    return ESP_OK;
}

esp_err_t ds2482_write_byte(uint8_t byte) {
    uint8_t cmd[2] = { DS2482_CMD_WRITE_BYTE, byte };
    esp_err_t err = ds2482_busy_wait();
    if (err != ESP_OK) return err;
    return i2c_master_write_to_device(g_i2c_num, g_i2c_addr, cmd, 2, pdMS_TO_TICKS(100));
}

#define DS2482_REG_DATA           0xE1  // registro de datos del DS2482

esp_err_t ds2482_read_byte(uint8_t *data) {
    uint8_t cmd = DS2482_CMD_READ_BYTE;
    esp_err_t err;
    err = ds2482_busy_wait();
    if (err != ESP_OK) return err;
    err = i2c_master_write_to_device(g_i2c_num, g_i2c_addr, &cmd, 1, pdMS_TO_TICKS(100));
    if (err != ESP_OK) return err;
    err = ds2482_busy_wait();
    if (err != ESP_OK) return err;
    // Apuntar al registro de DATOS antes de leer — sin esto se lee status (0xF0)
    uint8_t set_ptr[2] = { DS2482_CMD_SET_READ_PTR, DS2482_REG_DATA };
    err = i2c_master_write_to_device(g_i2c_num, g_i2c_addr, set_ptr, 2, pdMS_TO_TICKS(100));
    if (err != ESP_OK) return err;
    return i2c_master_read_from_device(g_i2c_num, g_i2c_addr, data, 1, pdMS_TO_TICKS(100));
}

esp_err_t ds2482_1wire_triplet(uint8_t direction, uint8_t *status) {
    uint8_t cmd[2] = { DS2482_CMD_1WIRE_TRIPLET, (direction ? 0x80 : 0x00) };
    esp_err_t err;
    err = ds2482_busy_wait();
    if (err != ESP_OK) return err;
    err = i2c_master_write_to_device(g_i2c_num, g_i2c_addr, cmd, 2, pdMS_TO_TICKS(100));
    if (err != ESP_OK) return err;
    err = ds2482_busy_wait();
    if (err != ESP_OK) return err;
    ds2482_set_read_pointer(DS2482_REG_STATUS);
    return ds2482_read_register(status);
}

esp_err_t ds2482_search_rom(uint64_t *rom_code) {
    *rom_code = 0;
    esp_err_t err;

    bool presence;
    err = ds2482_1wire_reset(&presence);
    if (err != ESP_OK) return err;
    if (!presence) {
        ESP_LOGW(TAG, "No 1-Wire device present");
        return ESP_ERR_NOT_FOUND;
    }

    err = ds2482_write_byte(0xF0);  // Search ROM
    if (err != ESP_OK) return err;

    for (int bit = 0; bit < 64; bit++) {
        uint8_t direction = 0;
        uint8_t status;
        err = ds2482_1wire_triplet(direction, &status);
        if (err != ESP_OK) return err;

        if ((status & DS2482_STATUS_SBR) && (status & DS2482_STATUS_TSB)) {
            ESP_LOGE(TAG, "ROM search conflict");
            return ESP_FAIL;
        }

        uint8_t bit_value = (status & DS2482_STATUS_DIR) ? 1 : 0;
        *rom_code |= ((uint64_t)bit_value << bit);
    }

    return ESP_OK;
}

esp_err_t ds2482_read_status(ds2482_t *dev, uint8_t *status) {
    g_i2c_num = dev->i2c_num;
    g_i2c_addr = dev->address;
    esp_err_t err = ds2482_set_read_pointer(DS2482_REG_STATUS);
    if (err != ESP_OK) return err;
    return ds2482_read_register(status);
}

esp_err_t ds2482_search_rom_all(uint64_t *roms, size_t max_devices, size_t *found) {
    *found = 0;
    uint64_t last_rom = 0;
    int last_discrepancy = 0;
    int last_device_flag = 0;
    esp_err_t err;

    while (!last_device_flag && *found < max_devices) {
        uint64_t rom = 0;
        int current_discrepancy = 0;
        int bit_number = 1;
        bool conflict = false;

        // Hasta 3 intentos por iteración — bus largo puede necesitar recuperación
        for (int retry = 0; retry < 3; retry++) {
            rom = 0;
            current_discrepancy = 0;
            bit_number = 1;
            conflict = false;

            bool presence;
            err = ds2482_1wire_reset(&presence);
            if (err != ESP_OK) return err;
            if (!presence) {
                ESP_LOGW(TAG, "No hay dispositivos 1-Wire presentes");
                return ESP_ERR_NOT_FOUND;
            }

            err = ds2482_write_byte(0xF0); // Search ROM
            if (err != ESP_OK) return err;

            // Delay extra post-comando: con 80m de cable los esclavos necesitan
            // tiempo para decodificar 0xF0 y preparar su primer bit de salida.
            vTaskDelay(pdMS_TO_TICKS(20));

            for (int byte_idx = 0; byte_idx < 8; byte_idx++) {
                uint8_t byte_val = 0;

                for (int bit = 0; bit < 8; bit++, bit_number++) {
                    uint8_t search_direction;

                    if (bit_number < last_discrepancy) {
                        search_direction = (last_rom >> (bit_number - 1)) & 0x01;
                    } else if (bit_number == last_discrepancy) {
                        search_direction = 1;
                    } else {
                        search_direction = 0;
                    }

                    uint8_t status;
                    err = ds2482_1wire_triplet(search_direction, &status);
                    if (err != ESP_OK) return err;

                    uint8_t id_bit     = (status & DS2482_STATUS_SBR) ? 1 : 0;
                    uint8_t cmp_id_bit = (status & DS2482_STATUS_TSB) ? 1 : 0;
                    uint8_t branch_dir = (status & DS2482_STATUS_DIR) ? 1 : 0;

                    if (id_bit && cmp_id_bit) {
                        // (1,1): ningún esclavo respondió — bus inestable.
                        // 1-Wire reset (NO device reset) para limpiar el bus
                        // sin perder la configuración APU del DS2482.
                        ESP_LOGW(TAG, "Conflicto 1,1 en bit %d — reintento %d/3", bit_number, retry + 1);
                        bool dummy;
                        ds2482_1wire_reset(&dummy);
                        vTaskDelay(pdMS_TO_TICKS(50)); // 50ms: bus capacitivo se estabiliza
                        conflict = true;
                        break;
                    }

                    if (!id_bit && !cmp_id_bit && search_direction == 0) {
                        current_discrepancy = bit_number;
                    }

                    byte_val |= (branch_dir << bit);
                }

                if (conflict) break;
                rom |= ((uint64_t)byte_val << (byte_idx * 8));
            }

            if (!conflict) break; // búsqueda exitosa: salir del loop de retries
        }

        if (conflict) {
            last_device_flag = 1; // después de 3 reintentos fallidos, detenemos
        } else {
            roms[*found] = rom;
            (*found)++;

            if (current_discrepancy == 0) {
                last_device_flag = 1;
            } else {
                last_discrepancy = current_discrepancy;
                last_rom = rom;
            }
        }
    }

    return ESP_OK;
}

// Comando Write Configuration del DS2482
#define DS2482_CMD_WRITE_CONFIG  0xD2

esp_err_t ds2482_configure(ds2482_t *dev, uint8_t config) {
    // Construir el byte de configuración con complemento en nibble alto
    uint8_t config_byte = DS2482_CFG_BYTE(config);

    uint8_t buf[2] = { DS2482_CMD_WRITE_CONFIG, config_byte };

    esp_err_t err = i2c_master_write_to_device(
        dev->i2c_num,
        dev->address,
        buf,
        sizeof(buf),
        pdMS_TO_TICKS(100)
    );

    if (err != ESP_OK) {
        ESP_LOGE("DS2482", "Error escribiendo configuración: %s", esp_err_to_name(err));
        return err;
    }

    // Leer de vuelta el registro de configuración para verificar
    // El DS2482 devuelve solo los 4 bits bajos si la escritura fue exitosa
    uint8_t readback = 0;
    err = ds2482_read_register(&readback);
    if (err != ESP_OK) return err;

    if ((readback & 0x0F) != (config & 0x0F)) {
        ESP_LOGE("DS2482", "Configuración rechazada por DS2482. Byte enviado: 0x%02X, recibido: 0x%02X",
                 config_byte, readback);
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI("DS2482", "Configuración aplicada: APU=%d SPU=%d 1WS=%d",
             (config & DS2482_CFG_APU) ? 1 : 0,
             (config & DS2482_CFG_SPU) ? 1 : 0,
             (config & DS2482_CFG_1WS) ? 1 : 0);

    return ESP_OK;
}