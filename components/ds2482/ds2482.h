#pragma once

#include <stdint.h> 
#include <stdbool.h> 
#include "esp_err.h"
#include "driver/i2c.h"

// Dirección I2C por defecto del DS2482-100 (AD0 y AD1 conectados a GND)
#define DS2482_I2C_ADDR 0x18

typedef struct {
    i2c_port_t i2c_num;
    uint8_t address;
} ds2482_t;

esp_err_t ds2482_init(ds2482_t *dev, i2c_port_t i2c_num, uint8_t address);
esp_err_t ds2482_reset(ds2482_t *dev);
esp_err_t ds2482_1wire_reset(bool *presence);
esp_err_t ds2482_write_byte(uint8_t byte);
esp_err_t ds2482_read_byte(uint8_t *data);
esp_err_t ds2482_search_rom(uint64_t *rom_code);

esp_err_t ds2482_set_read_pointer(uint8_t reg);
esp_err_t ds2482_read_register(uint8_t *value);
esp_err_t ds2482_busy_wait();
esp_err_t ds2482_1wire_triplet(uint8_t direction, uint8_t *status);
esp_err_t ds2482_read_status(ds2482_t *dev, uint8_t *status);
esp_err_t ds2482_search_rom_all(uint64_t *roms, size_t max_devices, size_t *found);