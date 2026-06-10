#include <stdio.h>

/* Attributes SRVC Table — IDJ Programador (solo Jaulas) */
enum
{
    DEVICE_SVC,
    DEVICE_CHAR_ID,
    DEVICE_CHAR_VAL_ID,
    DEVICE_CHAR_WIFI_SSID,
    DEVICE_CHAR_VAL_WIFI_SSID,
    DEVICE_CHAR_WIFI_PSWD,
    DEVICE_CHAR_VAL_WIFI_PSWD,
    DEVICE_CHAR_JAULA,        // 0xA0B4 — Write número / "READ"
    DEVICE_CHAR_VAL_JAULA,
    DEVICE_CHAR_STATUS,       // 0xA0B5 — Notify respuesta
    DEVICE_CHAR_VAL_STATUS,
    DEVICE_CHAR_CFG_STATUS,
    DEVICE_IDX_NB,            // 12 atributos
};
