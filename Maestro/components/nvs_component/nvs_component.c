#include <stdio.h>
#include "nvs_component.h"

/// @brief Initialize NVS with GIO functionality
void init_nvs_component(){
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK( err );
    ESP_LOGI(NVS_TAG, " NVS Initialized");
}

/// @brief Writes a variable a value in NVS
/// @param var_name Variable name in NVS table
/// @param variable Vriable value to sotre in NVS table
/// @return The value stored
int32_t write_nvs(char *var_name, int32_t variable){

    ESP_LOGI(NVS_TAG, "Opening Non-Volatile Storage (NVS)");
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(NVS_TAG, "Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    }
    
    // Write
    ESP_LOGI(NVS_TAG, "Updating %s in NVS = %ld", var_name, variable);
    err = nvs_set_i32(my_handle, var_name, (int32_t)variable);
    if (err != ESP_OK){
        ESP_LOGE(NVS_TAG, "Failed");
        
    }
    
    ESP_LOGI(NVS_TAG, "Committing updates in NVS");
    err = nvs_commit(my_handle);
    if (err != ESP_OK){
        ESP_LOGE(NVS_TAG, "Failed");  
    }

    // Close
    nvs_close(my_handle);

    return variable;
}

/// @brief Read a variable name stored in NVS
/// @param var_name Variable name in NVS table
/// @param default_value Default value to save if the variable isn't initialized
/// @return Variable value stored with the variable name, ERROR -> `value = -1`
int32_t read_nvs(char *var_name, int32_t default_value){

    ESP_LOGI(NVS_TAG, "Opening Non-Volatile Storage (NVS) handle...");
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(NVS_TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
    }
    
    int32_t variable=-1; // variable to store NVS value
    err = nvs_get_i32(my_handle, var_name, &variable);
    switch (err) {
        case ESP_OK:
            ESP_LOGI(NVS_TAG, "Variable %s = %ld", var_name, variable);
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            ESP_LOGE(NVS_TAG, "%s is not initialized yet!", var_name);
            variable = write_nvs(var_name, default_value);
            break;
        default :
            ESP_LOGE(NVS_TAG, "Error reading %s!\n", esp_err_to_name(err));
    }

    // Close
    nvs_close(my_handle);
    
    return variable;
}

/// @brief Read a variable name stored in NVS
/// @param var_name Variable name in NVS table
/// @param output_buffer Buffer to store the result (should be allocated by the caller)
/// @param max_len Maximum length of the output buffer
/// @return ESP_OK if successful, otherwise an error code
esp_err_t read_str_nvs(const char *var_name, char *output_buffer, size_t max_len){
    ESP_LOGI(NVS_TAG, "Opening Non-Volatile Storage (NVS) handle...");
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(NVS_TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return err;  // Return the error code
    }
    
    size_t required_size = 0;
    err = nvs_get_str(my_handle, var_name, NULL, &required_size);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGE(NVS_TAG, "The value for %s is not initialized yet!", var_name);
        } else {
            ESP_LOGE(NVS_TAG, "Error (%s) reading the size of the variable!", esp_err_to_name(err));
        }
        nvs_close(my_handle);
        return err;  // Return the error code
    }

    if (required_size > max_len) {
        ESP_LOGE(NVS_TAG, "Buffer too small! Required: %zu, Provided: %zu", required_size, max_len);
        nvs_close(my_handle);
        return ESP_ERR_NO_MEM;  // Indicate buffer size is insufficient
    }

    char* temp_variable = malloc(required_size);
    if (temp_variable == NULL) {
        ESP_LOGE(NVS_TAG, "Failed to allocate memory for variable value!");
        nvs_close(my_handle);
        return ESP_ERR_NO_MEM;  // Return error on malloc failure
    }

    err = nvs_get_str(my_handle, var_name, temp_variable, &required_size);
    if (err == ESP_OK) {
        snprintf(output_buffer, max_len, "%s", temp_variable);  // Copy the string to the output buffer
        ESP_LOGI(NVS_TAG, "Variable %s value = %s", var_name, output_buffer);
    } else {
        ESP_LOGE(NVS_TAG, "Error (%s) reading the value of the variable!", esp_err_to_name(err));
    }

    free(temp_variable);  // Free the allocated memory
    nvs_close(my_handle);
    
    return err;  // Return the status of the operation
}


/// @brief Write a string to a specified variable name in NVS
/// @param var_name Variable name in NVS table
/// @param value String value to store
/// @return ESP_OK if successful, otherwise an error code
esp_err_t write_str_nvs(const char *var_name, const char *value){
    ESP_LOGI(NVS_TAG, "Opening Non-Volatile Storage (NVS) handle...");
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(NVS_TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return err;  // Return the error code
    }

    ESP_LOGI(NVS_TAG, "Writing value %s to variable %s...", value, var_name);
    err = nvs_set_str(my_handle, var_name, value);
    if (err != ESP_OK) {
        ESP_LOGE(NVS_TAG, "Failed to write value to NVS (%s)!", esp_err_to_name(err));
    } else {
        ESP_LOGI(NVS_TAG, "Committing changes in NVS...");
        err = nvs_commit(my_handle);
        if (err != ESP_OK) {
            ESP_LOGE(NVS_TAG, "Failed to commit changes to NVS (%s)!", esp_err_to_name(err));
        } else {
            ESP_LOGI(NVS_TAG, "Value %s successfully written to variable %s.", value, var_name);
        }
    }

    nvs_close(my_handle);
    return err;  // Return the status of the operation
}