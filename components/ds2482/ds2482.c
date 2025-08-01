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

esp_err_t ds2482_busy_wait() {
    uint8_t status;
    do {
        ds2482_set_read_pointer(DS2482_REG_STATUS);
        ds2482_read_register(&status);
        vTaskDelay(pdMS_TO_TICKS(1));
    } while (status & DS2482_STATUS_BUSY);
    return ESP_OK;
}

esp_err_t ds2482_1wire_reset(bool *presence) {
    uint8_t cmd = DS2482_CMD_1WIRE_RESET;
    ESP_ERROR_CHECK(ds2482_busy_wait());
    ESP_ERROR_CHECK(i2c_master_write_to_device(g_i2c_num, g_i2c_addr, &cmd, 1, pdMS_TO_TICKS(100)));
    ESP_ERROR_CHECK(ds2482_busy_wait());

    uint8_t status;
    ds2482_set_read_pointer(DS2482_REG_STATUS);
    ds2482_read_register(&status);

    *presence = (status & DS2482_STATUS_PPD) != 0;
    return ESP_OK;
}

esp_err_t ds2482_write_byte(uint8_t byte) {
    uint8_t cmd[2] = { DS2482_CMD_WRITE_BYTE, byte };
    ESP_ERROR_CHECK(ds2482_busy_wait());
    return i2c_master_write_to_device(g_i2c_num, g_i2c_addr, cmd, 2, pdMS_TO_TICKS(100));
}

esp_err_t ds2482_read_byte(uint8_t *data) {
    uint8_t cmd = DS2482_CMD_READ_BYTE;
    ESP_ERROR_CHECK(ds2482_busy_wait());
    ESP_ERROR_CHECK(i2c_master_write_to_device(g_i2c_num, g_i2c_addr, &cmd, 1, pdMS_TO_TICKS(100)));
    vTaskDelay(pdMS_TO_TICKS(2));
    return i2c_master_read_from_device(g_i2c_num, g_i2c_addr, data, 1, pdMS_TO_TICKS(100));
}

esp_err_t ds2482_1wire_triplet(uint8_t direction, uint8_t *status) {
    uint8_t cmd[2] = { DS2482_CMD_1WIRE_TRIPLET, (direction ? 0x80 : 0x00) };
    ESP_ERROR_CHECK(ds2482_busy_wait());
    ESP_ERROR_CHECK(i2c_master_write_to_device(g_i2c_num, g_i2c_addr, cmd, 2, pdMS_TO_TICKS(100)));
    ESP_ERROR_CHECK(ds2482_busy_wait());
    ds2482_set_read_pointer(DS2482_REG_STATUS);
    return ds2482_read_register(status);
}

esp_err_t ds2482_search_rom(uint64_t *rom_code) {
    *rom_code = 0;

    bool presence;
    ESP_ERROR_CHECK(ds2482_1wire_reset(&presence));
    if (!presence) {
        ESP_LOGW(TAG, "No 1-Wire device present");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_ERROR_CHECK(ds2482_write_byte(0xF0));  // Search ROM

    for (int bit = 0; bit < 64; bit++) {
        uint8_t direction = 0; // siempre 0 en esta versión
        uint8_t status;
        ESP_ERROR_CHECK(ds2482_1wire_triplet(direction, &status));

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
    // Actualiza variables globales para que las internas funcionen
    g_i2c_num = dev->i2c_num;
    g_i2c_addr = dev->address;
    ESP_ERROR_CHECK(ds2482_set_read_pointer(DS2482_REG_STATUS));
    return ds2482_read_register(status);
}

esp_err_t ds2482_search_rom_all(uint64_t *roms, size_t max_devices, size_t *found) {
    *found = 0;
    uint64_t last_rom = 0;
    int last_discrepancy = 0;
    int last_device_flag = 0;

    while (!last_device_flag && *found < max_devices) {
        uint64_t rom = 0;
        int current_discrepancy = 0;
        int bit_number = 1;

        bool presence;
        ESP_ERROR_CHECK(ds2482_1wire_reset(&presence));
        if (!presence) {
            ESP_LOGW(TAG, "No hay dispositivos 1-Wire presentes");
            return ESP_ERR_NOT_FOUND;
        }

        ESP_ERROR_CHECK(ds2482_write_byte(0xF0)); // Search ROM

        for (int byte = 0; byte < 8; byte++) {
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
                ESP_ERROR_CHECK(ds2482_1wire_triplet(search_direction, &status));

                uint8_t id_bit = (status & DS2482_STATUS_SBR) ? 1 : 0;
                uint8_t cmp_id_bit = (status & DS2482_STATUS_TSB) ? 1 : 0;
                uint8_t branch_direction = (status & DS2482_STATUS_DIR) ? 1 : 0;

                if (id_bit && cmp_id_bit) {
                    ESP_LOGE(TAG, "Conflicto no resoluble");
                    return ESP_FAIL;
                }

                if (!id_bit && !cmp_id_bit && search_direction == 0) {
                    current_discrepancy = bit_number;
                }

                byte_val |= (branch_direction << bit);
            }

            rom |= ((uint64_t)byte_val << (byte * 8));
        }

        roms[*found] = rom;
        (*found)++;

        if (current_discrepancy == 0) {
            last_device_flag = 1;
        } else {
            last_discrepancy = current_discrepancy;
            last_rom = rom;
        }
    }

    return ESP_OK;
}