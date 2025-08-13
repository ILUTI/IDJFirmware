#ifndef GIO_NVS_H
#define GIO_NVS_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "nvs_flash.h"
#include "nvs.h"

#include "esp_system.h"
#include "esp_log.h"


#define NVS_TAG     "NVS"

/// @brief Initialize NVS
void init_nvs_component();

/// @brief Writes a variable a value in NVS
/// @param var_name Variable name in NVS table
/// @param variable Vriable value to sotre in NVS table
/// @return The value stored
int32_t write_nvs(char *var_name, int32_t variable);

/// @brief Read a variable name stored in NVS
/// @param var_name Variable name in NVS table
/// @param default_value Default value to save if the variable isn't initialized
/// @return Variable value stored with the variable name, ERROR -> `value = -1`
int32_t read_nvs(char *var_name, int32_t default_value);

/// @brief Read a variable name stored in NVS
/// @param var_name Variable name in NVS table
/// @param output_buffer Buffer to store the result (should be allocated by the caller)
/// @param max_len Maximum length of the output buffer
/// @return ESP_OK if successful, otherwise an error code
esp_err_t read_str_nvs(const char *var_name, char *output_buffer, size_t max_len);

/// @brief Write a string to a specified variable name in NVS
/// @param var_name Variable name in NVS table
/// @param value String value to store
/// @return ESP_OK if successful, otherwise an error code
esp_err_t write_str_nvs(const char *var_name, const char *value);

#endif // GIO_NVS_H  // End of the include guard