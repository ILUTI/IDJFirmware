idf_component_register(
    SRCS "wifi_component.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_component mqtt_component ble_component esp_wifi 
    )
