#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/i2c.h"
#include "driver/uart.h"
#include "nvs_component.h"
#include "ds2482.h"
#include "ds2431.h"

#define TAG              "IDJ-PROG"
#define I2C_SCL_IO       5
#define I2C_SDA_IO       4
#define I2C_NUM          I2C_NUM_0
#define I2C_FREQ_HZ      100000
#define UART_BUF_SIZE    256

// Genera "T0603-xxxx" a partir del número
void generar_unidad(uint16_t jaula, char *out) {
    snprintf(out, 12, "T0603-%04d", jaula);
}

// Lee una línea del serial. Retorna true si recibió input.
bool leer_linea(char *buf, size_t max) {
    size_t pos = 0;
    memset(buf, 0, max);
    while (1) {
        uint8_t c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(100)) <= 0) continue;
        uart_write_bytes(UART_NUM_0, (const char *)&c, 1);
        if (c == '\r' || c == '\n') {
            buf[pos] = '\0';
            printf("\n");
            return true; // <-- retorna siempre, con o sin texto
        } else if ((c == 8 || c == 127) && pos > 0) {
            pos--;
            uart_write_bytes(UART_NUM_0, "\b \b", 3);
        } else if (pos < max - 1) {
            buf[pos++] = (char)c;
        }
    }
}

// ── Imprime los bytes crudos de la EEPROM en formato tabla ──────
void imprimir_eeprom_cruda(uint8_t *raw, size_t len) {
    printf("\n  Addr  | 00 01 02 03 04 05 06 07\n");
    printf("  ------+------------------------\n");
    for (size_t i = 0; i < len; i += 8) {
        printf("  0x%02X  | ", (unsigned int)i);
        for (size_t j = i; j < i + 8 && j < len; j++) {
            printf("%02X ", raw[j]);
        }
        printf("\n");
    }
}

// ── Lee y verifica la EEPROM del esclavo, muestra resultado ─────
bool verificar_eeprom(ds2482_t *ds2482, ds2431_t *esclavo) {
    printf("\n--- Verificacion EEPROM ---\n");
    uint8_t raw[DS2431_EEPROM_DATA_LEN];
    esp_err_t err = ds2431_read_memory(ds2482, esclavo, 0x00, raw, sizeof(raw));
    if (err != ESP_OK) {
        printf("  ❌ ERROR de comunicación: No se pudo leer la memoria (%s)\n", esp_err_to_name(err));
        return false;
    }

    imprimir_eeprom_cruda(raw, sizeof(raw));

    // Validación de los datos
    ds2431_data_t leido;
    err = ds2431_leer_datos(ds2482, esclavo, &leido);

    if (err != ESP_OK || !leido.valido) {
        // Análisis del fallo para el usuario
        if (raw[0] == 0xFF && raw[1] == 0xFF) {
            printf("  Estado: EEPROM VIRGEN (Sin programar)\n");
        } else {
            uint16_t crc_calc = ds2431_crc16(raw, 20);
            uint16_t crc_stored = (uint16_t)raw[20] | ((uint16_t)raw[21] << 8);
            printf("  Estado: ❌ DATOS CORRUPTOS\n");
            printf("  CRC: Almacenado=0x%04X | Calculado=0x%04X -> FALLO\n", crc_stored, crc_calc);
        }
        return false; // Retorna falso si hay cualquier error
    }

    // Si llegamos aquí, los datos son válidos
    printf("  [0x00-0x01] Magic     : OK ('ID')\n");
    printf("  [0x02-0x03] Jaula     : #%d\n", leido.numero_jaula);
    printf("  [0x04-0x0F] Unidad    : '%s'\n", leido.unidad);
    printf("  [0x10-0x13] Timestamp : %lu\n", leido.timestamp);
    printf("  [0x14-0x15] CRC-16    : OK (0x%04X)\n", ds2431_crc16(raw, 20));

    printf("\n  ✅ RESULTADO: Jaula #%d verificada correctamente en hardware.\n", leido.numero_jaula);
    return true; // Éxito total[cite: 1, 2]
}

void app_main(void) {
    nvs_flash_init();

    // I2C
    i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = I2C_SDA_IO,
        .scl_io_num       = I2C_SCL_IO,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };
    i2c_param_config(I2C_NUM, &conf);
    i2c_driver_install(I2C_NUM, conf.mode, 0, 0, 0);

    // UART
    uart_config_t uart_cfg = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(UART_NUM_0, &uart_cfg);
    uart_driver_install(UART_NUM_0, UART_BUF_SIZE * 2, 0, 0, NULL, 0);

    // DS2482
    ds2482_t ds2482;
    if (ds2482_init(&ds2482, I2C_NUM, DS2482_I2C_ADDR) != ESP_OK) {
        ESP_LOGE(TAG, "DS2482 no detectado"); return;
    }
    ds2482_configure(&ds2482, DS2482_CFG_APU);

    printf("\n=== IDJ Programador ===\n");
    printf("Comandos: ENTER = programar | 'r' = solo leer EEPROM | 'q' = salir\n\n");

    char buf[16];

    while (1) {
        // Detectar esclavo
        bool presence = false;
        ds2482_1wire_reset(&presence);
        if (!presence) {
            printf("Sin esclavo en bus. Conecta uno y presiona ENTER...\n");
            leer_linea(buf, sizeof(buf));
            continue;
        }

        uint64_t roms[2];
        size_t found = 0;
        ds2482_search_rom_all(roms, 2, &found);

        if (found != 1) {
            printf("Se detectaron %d esclavos. Deja solo uno conectado.\n", (int)found);
            leer_linea(buf, sizeof(buf));
            continue;
        }

        // Mostrar ROM detectado
        uint8_t *b = (uint8_t *)&roms[0];
        printf("ROM: ");
        for (int i = 0; i < 8; i++) printf("%02X", b[i]);
        printf("\n");

        ds2431_t esclavo = { .rom_code = roms[0] };

        // Comando
        printf("Accion (ENTER=programar | r=leer | q=salir): ");
        fflush(stdout);
        leer_linea(buf, sizeof(buf));

        // ── Solo lectura ─────────────────────────────────────────
        if (buf[0] == 'r' || buf[0] == 'R') {
            verificar_eeprom(&ds2482, &esclavo);
            printf("\n");
            continue;
        }

        if (buf[0] == 'q' || buf[0] == 'Q') {
            printf("Saliendo.\n");
            break;
        }

        // ── Programar ────────────────────────────────────────────
        printf("Numero de jaula (1-250): ");
        fflush(stdout);
        leer_linea(buf, sizeof(buf));

        long val = strtol(buf, NULL, 10);
        if (val < 1 || val > 250) {
            printf("Numero invalido.\n");
            continue;
        }

        // Escribir EEPROM
        char unidad[12];
        generar_unidad((uint16_t)val, unidad);

        //ds2431_t esclavo = { .rom_code = roms[0] };
        ds2431_data_t datos = {
            .numero_jaula = (uint16_t)val,
            .timestamp    = 0,
            .valido       = false,
        };
        strncpy(datos.unidad, unidad, 11);
        datos.unidad[11] = '\0';

        esp_err_t err = ds2431_escribir_datos(&ds2482, &esclavo, &datos);
        // Verificacion de la escritura y de la comunicación con el dispositivo
        if (err != ESP_OK) {
            printf("❌ ERROR DE COMUNICACIÓN: %s\n", esp_err_to_name(err));
            continue;
        }

        printf("Escritura finalizada. Iniciando comprobación física...\n");
        
        // Solo si la verificación física pasa, damos el visto bueno para poder escribir
        if (verificar_eeprom(&ds2482, &esclavo)) {
            printf("✅ TODO OK — Jaula lista. Puedes reconectar al chain.\n\n");
        } else {
            printf("❌ ERROR: La grabación falló o los datos están corruptos. NO DESCONECTAR.\n\n");
        }
    }
}