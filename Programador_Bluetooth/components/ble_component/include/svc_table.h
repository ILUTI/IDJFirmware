#include <stdio.h>

/* Attributes SRVC Table — IDJ Programador v2 (con soporte Dolly) */
enum
{
    DEVICE_SVC,

    DEVICE_CHAR_ID,
    DEVICE_CHAR_VAL_ID,

    DEVICE_CHAR_WIFI_SSID,
    DEVICE_CHAR_VAL_WIFI_SSID,

    DEVICE_CHAR_WIFI_PSWD,
    DEVICE_CHAR_VAL_WIFI_PSWD,

    // ── Número de Jaula (Write = programa, Write "READ" = lee EEPROM) ──
    DEVICE_CHAR_JAULA,
    DEVICE_CHAR_VAL_JAULA,

    // ── Número de Dolly (Write = guarda pendiente, "0"/"NONE" = sin dolly) ──
    DEVICE_CHAR_DOLLY,          // NUEVO
    DEVICE_CHAR_VAL_DOLLY,      // NUEVO

    // ── Canal de respuesta (Read + Notify) ──
    DEVICE_CHAR_STATUS,
    DEVICE_CHAR_VAL_STATUS,
    DEVICE_CHAR_CFG_STATUS,

    DEVICE_IDX_NB,              // Total: 14 atributos
};
