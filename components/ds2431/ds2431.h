#pragma once
#include <stdint.h>
#include "ds2482.h"

typedef struct {
    uint64_t rom_code;
} ds2431_t;

esp_err_t ds2431_read_memory(ds2482_t *ds2482, ds2431_t *dev, uint16_t addr, uint8_t *data, size_t len);