#include <stdio.h>
#include "ds2482.h"
#include "ds2431.h"
#include "driver/i2c.h"

//Tabla para mapeo
typedef struct {
    uint64_t rom;
    const char *unidad;
} dispositivo_info_t;

dispositivo_info_t dispositivos_registrados[] = {
    {0xEC000048F3EA902D, "T0603-0001"},
    {0x5D000048F3FFF42D, "T0603-0002"},
    {0x28000048F45F022D, "T0603-0003"},
    {0x84000048F49AA22D, "T0603-0004"},
    {0xF0000048F45E392D, "T0603-0005"},
    {0xB6000048F472F92D, "T0603-0006"}
};

//Funcion para mapeo
const size_t num_dispositivos_registrados = sizeof(dispositivos_registrados) / sizeof(dispositivo_info_t);

const char* buscar_unidad_por_rom(uint64_t rom) {
    for (size_t i = 0; i < num_dispositivos_registrados; i++) {
        if (dispositivos_registrados[i].rom == rom) {
            return dispositivos_registrados[i].unidad;
        }
    }
    return "Desconocido";
}

#define I2C_MASTER_SCL_IO 10  // Cambia aquí el pin SCL (IO10)
#define I2C_MASTER_SDA_IO 11  // Cambia aquí el pin SDA (IO11)
#define I2C_MASTER_NUM I2C_NUM_0
#define I2C_MASTER_FREQ_HZ 100000

void app_main(void) {
    // Configuración de I2C
        i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    i2c_param_config(I2C_MASTER_NUM, &conf);
    i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);

    ds2482_t ds2482;
    esp_err_t err = ds2482_init(&ds2482, I2C_MASTER_NUM, DS2482_I2C_ADDR);
    if (err != ESP_OK) {
        printf("No se detecta el DS2482 en el bus I2C.\n");
        return;
    }

    uint8_t status = 0;
    err = ds2482_read_status(&ds2482, &status);
    if (err != ESP_OK) {
        printf("Error leyendo el registro de estado del DS2482.\n");
        return;
    }
    printf("Registro de estado DS2482: 0x%02X\n", status);

    bool presence = false;
    err = ds2482_1wire_reset(&presence);
    if (err != ESP_OK) {
        printf("Error en reset 1-Wire\n");
        return;
    }
    if (presence) {
        printf("Dispositivo 1-Wire detectado (presencia=1)\n");
    } else {
        printf("No se detectó dispositivo 1-Wire (presencia=0)\n");
    }

    //Detecta solo un dispositivo 1-Wire
    /*
    uint64_t rom_code = 0;
    err = ds2482_search_rom(&rom_code);
    if (err == ESP_OK) {
        printf("Dirección ROM del dispositivo 1-Wire: 0x%016llX\n", rom_code);
    } else {
        printf("Error buscando la dirección ROM del dispositivo 1-Wire.\n");
    }
    */

    //Escaneo de ROM's
    /*
    while (1)
    {
    uint64_t roms[5];
    size_t found = 0;
    err = ds2482_search_rom_all(roms, 5, &found);
    if (err == ESP_OK) {
        printf("Se encontraron %zu dispositivos:\n", found);
        for (size_t i = 0; i < found; i++) {
            printf("Dispositivo %zu ROM: 0x%016llX\n", i + 1, roms[i]);
            }
    } else {
            printf("Error buscando dispositivos 1-Wire.\n");
        }
        vTaskDelay(pdMS_TO_TICKS(5000)); // Espera 5 segundos antes de la siguiente iteración
    }
    */

    //Mapeo de unidades y match con ROM's
    while(1){
        uint64_t roms[5];
        size_t found = 0;
        err = ds2482_search_rom_all(roms, 5, &found);
        for (size_t i = 0; i < found; i++) {
        const char *unidad = buscar_unidad_por_rom(roms[i]);
        printf("Dispositivo %zu ROM: 0x%016llX - Unidad: %s\n", i + 1, roms[i], unidad);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}