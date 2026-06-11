#include <stdio.h>
#include "ble_component.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Cola declarada en main_programador.c
extern QueueHandle_t cola_programacion_jaula;

// ── Variables compartidas con main_programador.c ──────────────────────────────
volatile bool     ble_cmd_leer      = false; // Flag: ejecutar lectura de EEPROM
// ─────────────────────────────────────────────────────────────────────────────

static uint8_t adv_config_done = 0;
static uint16_t device_handle_table[DEVICE_IDX_NB];

// Variables de conexión BLE
uint16_t ble_gatts_if  = ESP_GATT_IF_NONE;
uint16_t ble_conn_id   = 0xffff;
bool     ble_is_connected = false;

// ── UUIDs ─────────────────────────────────────────────────────────────────────
static uint8_t service_uuid[16] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00,
};

static const uint16_t GATTS_SERVICE_UUID        = 0xFF00;
static const uint16_t GATTS_CHAR_UUID_ID        = 0xFF0A;
static const uint16_t GATTS_CHAR_UUID_WIFI_SSID = 0xA0B2;
static const uint16_t GATTS_CHAR_UUID_WIFI_PSWD = 0xA0B3;
static const uint16_t GATTS_CHAR_UUID_JAULA      = 0xA0B4;
static const uint16_t GATTS_CHAR_UUID_STATUS     = 0xA0B5;
// ── Propiedades de características ───────────────────────────────────────────
static const uint16_t primary_service_uuid         = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t character_declaration_uuid   = ESP_GATT_UUID_CHAR_DECLARE;
static const uint8_t  char_prop_read_write         = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_READ;
static const uint16_t character_client_config_uuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
static const uint8_t  char_prop_read_notify        = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static const uint8_t  notification_ccc[2]          = {0x01, 0x00};
static const uint8_t  char_value[4]                = {0x11, 0x22, 0x33, 0x44};

// ── Datos de advertising ──────────────────────────────────────────────────────
static esp_ble_adv_data_t adv_data = {
    .set_scan_rsp        = false,
    .include_name        = true,
    .include_txpower     = true,
    .min_interval        = 0x0006,
    .max_interval        = 0x0010,
    .appearance          = 0x00,
    .manufacturer_len    = 0,
    .p_manufacturer_data = NULL,
    .service_data_len    = 0,
    .p_service_data      = NULL,
    .service_uuid_len    = sizeof(service_uuid),
    .p_service_uuid      = service_uuid,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_data_t scan_rsp_data = {
    .set_scan_rsp        = true,
    .include_name        = true,
    .include_txpower     = true,
    .min_interval        = 0x0006,
    .max_interval        = 0x0010,
    .appearance          = 0x00,
    .manufacturer_len    = 0,
    .p_manufacturer_data = NULL,
    .service_data_len    = 0,
    .p_service_data      = NULL,
    .service_uuid_len    = sizeof(service_uuid),
    .p_service_uuid      = service_uuid,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_params_t adv_params = {
    .adv_int_min       = 0x20,
    .adv_int_max       = 0x40,
    .adv_type          = ADV_TYPE_IND,
    .own_addr_type     = BLE_ADDR_TYPE_PUBLIC,
    .channel_map       = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

// ── Perfil GATT ───────────────────────────────────────────────────────────────
struct gatts_profile_inst {
    esp_gatts_cb_t   gatts_cb;
    uint16_t         gatts_if;
    uint16_t         app_id;
    uint16_t         conn_id;
    uint16_t         service_handle;
    esp_gatt_srvc_id_t service_id;
    uint16_t         char_handle;
    esp_bt_uuid_t    char_uuid;
    esp_gatt_perm_t  perm;
    esp_gatt_char_prop_t property;
    uint16_t         descr_handle;
    esp_bt_uuid_t    descr_uuid;
};

static void gatts_profile_event_handler(esp_gatts_cb_event_t event,
                                        esp_gatt_if_t gatts_if,
                                        esp_ble_gatts_cb_param_t *param);

static struct gatts_profile_inst device_profile_tab[PROFILE_NUM] = {
    [PROFILE_APP_IDX] = {
        .gatts_cb  = gatts_profile_event_handler,
        .gatts_if  = ESP_GATT_IF_NONE,
    },
};

// ── Tabla de atributos GATT ───────────────────────────────────────────────────
static const esp_gatts_attr_db_t gatt_db[DEVICE_IDX_NB] =
{
    // Declaración de servicio
    [DEVICE_SVC] =
    {{ESP_GATT_RSP_BY_APP}, {ESP_UUID_LEN_16, (uint8_t *)&primary_service_uuid, ESP_GATT_PERM_READ,
      sizeof(uint16_t), sizeof(GATTS_SERVICE_UUID), (uint8_t *)&GATTS_SERVICE_UUID}},

    // ID del dispositivo (Read/Write)
    [DEVICE_CHAR_ID] =
    {{ESP_GATT_RSP_BY_APP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
      CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_read_write}},
    [DEVICE_CHAR_VAL_ID] =
    {{ESP_GATT_RSP_BY_APP}, {ESP_UUID_LEN_16, (uint8_t *)&GATTS_CHAR_UUID_ID, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
      GATTS_DEMO_CHAR_VAL_LEN_MAX, sizeof(char_value), (uint8_t *)char_value}},

    // WiFi SSID (Read/Write)
    [DEVICE_CHAR_WIFI_SSID] =
    {{ESP_GATT_RSP_BY_APP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
      CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_read_write}},
    [DEVICE_CHAR_VAL_WIFI_SSID] =
    {{ESP_GATT_RSP_BY_APP}, {ESP_UUID_LEN_16, (uint8_t *)&GATTS_CHAR_UUID_WIFI_SSID, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
      GATTS_DEMO_CHAR_VAL_LEN_MAX, sizeof(char_value), (uint8_t *)char_value}},

    // WiFi Password (Read/Write)
    [DEVICE_CHAR_WIFI_PSWD] =
    {{ESP_GATT_RSP_BY_APP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
      CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_read_write}},
    [DEVICE_CHAR_VAL_WIFI_PSWD] =
    {{ESP_GATT_RSP_BY_APP}, {ESP_UUID_LEN_16, (uint8_t *)&GATTS_CHAR_UUID_WIFI_PSWD, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
      GATTS_DEMO_CHAR_VAL_LEN_MAX, sizeof(char_value), (uint8_t *)char_value}},

    // Número de Jaula 0xA0B4 (Write = programa | Write "READ" = lee EEPROM)
    [DEVICE_CHAR_JAULA] =
    {{ESP_GATT_RSP_BY_APP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
      CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_read_write}},
    [DEVICE_CHAR_VAL_JAULA] =
    {{ESP_GATT_RSP_BY_APP}, {ESP_UUID_LEN_16, (uint8_t *)&GATTS_CHAR_UUID_JAULA, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
      GATTS_DEMO_CHAR_VAL_LEN_MAX, sizeof(char_value), (uint8_t *)char_value}},

// Canal de respuesta 0xA0B5 (Read + Notify)
    [DEVICE_CHAR_STATUS] =
    {{ESP_GATT_RSP_BY_APP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
      CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_read_notify}},
    [DEVICE_CHAR_VAL_STATUS] =
    {{ESP_GATT_RSP_BY_APP}, {ESP_UUID_LEN_16, (uint8_t *)&GATTS_CHAR_UUID_STATUS, ESP_GATT_PERM_READ,
      GATTS_DEMO_CHAR_VAL_LEN_MAX, sizeof(char_value), (uint8_t *)char_value}},
    [DEVICE_CHAR_CFG_STATUS] =
    {{ESP_GATT_RSP_BY_APP}, {ESP_UUID_LEN_16, (uint8_t *)&character_client_config_uuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
      sizeof(notification_ccc), sizeof(notification_ccc), (uint8_t *)notification_ccc}},
};

// ── Read event handler ────────────────────────────────────────────────────────
static void handle_read_event(esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param) {
    esp_gatt_rsp_t rsp = {0};
    esp_err_t err;
    char value[16];

    switch (param->read.handle - 40)
    {
    case DEVICE_CHAR_VAL_ID:
        ESP_LOGI(BLE_TAG, "READ: ID");
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        sprintf(value, "%s-%02X%02X", CONFIG_DEVICE_NAME, mac[4], mac[5]);
        rsp.attr_value.handle = param->read.handle;
        rsp.attr_value.len    = strlen(value);
        memcpy(rsp.attr_value.value, value, rsp.attr_value.len);
        esp_ble_gatts_send_response(gatts_if, param->read.conn_id,
                                    param->read.trans_id, ESP_GATT_OK, &rsp);
        break;

    case DEVICE_CHAR_VAL_WIFI_SSID:
        ESP_LOGI(BLE_TAG, "READ: WiFi SSID");
        err = read_str_nvs("wifi_ssid", value, sizeof(value));
        if (err != ESP_OK) { ESP_LOGE(BLE_TAG, "Error leyendo SSID de NVS"); break; }
        rsp.attr_value.handle = param->read.handle;
        rsp.attr_value.len    = strlen(value);
        memcpy(rsp.attr_value.value, value, rsp.attr_value.len);
        esp_ble_gatts_send_response(gatts_if, param->read.conn_id,
                                    param->read.trans_id, ESP_GATT_OK, &rsp);
        break;

    case DEVICE_CHAR_VAL_WIFI_PSWD:
        ESP_LOGI(BLE_TAG, "READ: WiFi PSWD");
        err = read_str_nvs("wifi_pswd", value, sizeof(value));
        if (err != ESP_OK) { ESP_LOGE(BLE_TAG, "Error leyendo PSWD de NVS"); break; }
        rsp.attr_value.handle = param->read.handle;
        rsp.attr_value.len    = strlen(value);
        memcpy(rsp.attr_value.value, value, rsp.attr_value.len);
        esp_ble_gatts_send_response(gatts_if, param->read.conn_id,
                                    param->read.trans_id, ESP_GATT_OK, &rsp);
        break;

    default:
        rsp.attr_value.handle  = param->read.handle;
        rsp.attr_value.len     = 1;
        rsp.attr_value.value[0] = 0;
        esp_ble_gatts_send_response(gatts_if, param->read.conn_id,
                                    param->read.trans_id, ESP_GATT_OK, &rsp);
        break;
    }
}

// ── Write event handler ───────────────────────────────────────────────────────
static void handle_write_event(esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param) {
    ESP_LOGI(BLE_TAG, "WRITE_EVT handle=%d len=%d", param->write.handle, param->write.len);
    ESP_LOG_BUFFER_HEX(BLE_TAG, param->write.value, param->write.len);

    // Copiar valor como string null-terminated
    uint16_t len = (param->write.len < GATTS_DEMO_CHAR_VAL_LEN_MAX)
                   ? param->write.len
                   : GATTS_DEMO_CHAR_VAL_LEN_MAX;
    char value[GATTS_DEMO_CHAR_VAL_LEN_MAX + 1];
    memcpy(value, param->write.value, len);
    value[len] = '\0';

    switch (param->write.handle - 40)
    {
    case DEVICE_CHAR_VAL_ID:
        esp_restart();
        break;

    case DEVICE_CHAR_VAL_WIFI_SSID:
        ESP_LOGI(BLE_TAG, "WRITE: WiFi SSID → %s", value);
        write_str_nvs("wifi_ssid", value);
        break;

    case DEVICE_CHAR_VAL_WIFI_PSWD:
        ESP_LOGI(BLE_TAG, "WRITE: WiFi PSWD → %s", value);
        write_str_nvs("wifi_pswd", value);
        break;

    // ── Número de Jaula (0xA0B4) ─────────────────────────────────────────────
    // Escribir un número (1-9999) → encola la orden de programación.
    // Escribir "READ"            → activa el flag de lectura de EEPROM.
    case DEVICE_CHAR_VAL_JAULA:
        if (strcmp(value, "READ") == 0) {
            ESP_LOGI(BLE_TAG, "WRITE: Jaula → COMANDO LEER EEPROM");
            ble_cmd_leer = true;
        } else {
            uint16_t num_jaula = (uint16_t)atoi(value);
            if (num_jaula >= 1 && num_jaula <= 9999) {
                ESP_LOGI(BLE_TAG, "WRITE: Jaula → #%d", num_jaula);
                if (cola_programacion_jaula != NULL) {
                    xQueueSend(cola_programacion_jaula, &num_jaula, 0);
                }
            } else {
                ESP_LOGE(BLE_TAG, "Número de jaula inválido: '%s' (rango 1-9999)", value);
            }
        }
        break;

    default:
        break;
    }
}

// ── GAP event handler ─────────────────────────────────────────────────────────
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        adv_config_done &= (~ADV_CONFIG_FLAG);
        if (adv_config_done == 0) esp_ble_gap_start_advertising(&adv_params);
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        adv_config_done &= (~SCAN_RSP_CONFIG_FLAG);
        if (adv_config_done == 0) esp_ble_gap_start_advertising(&adv_params);
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS)
            ESP_LOGE(BLE_TAG, "Advertising start failed");
        else
            ESP_LOGI(BLE_TAG, "Advertising start OK");
        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS)
            ESP_LOGE(BLE_TAG, "Advertising stop failed");
        break;
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
        ESP_LOGI(BLE_TAG, "Conn params update: min=%d max=%d latency=%d timeout=%d",
                 param->update_conn_params.min_int, param->update_conn_params.max_int,
                 param->update_conn_params.latency,  param->update_conn_params.timeout);
        break;
    default:
        break;
    }
}

// ── GATTS profile event handler ───────────────────────────────────────────────
static void gatts_profile_event_handler(esp_gatts_cb_event_t event,
                                        esp_gatt_if_t gatts_if,
                                        esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTS_REG_EVT: {
        esp_err_t ret = esp_ble_gap_set_device_name(CONFIG_DEVICE_NAME);
        if (ret) ESP_LOGE(BLE_TAG, "set device name failed: %x", ret);
        ret = esp_ble_gap_config_adv_data(&adv_data);
        if (ret) ESP_LOGE(BLE_TAG, "config adv data failed: %x", ret);
        adv_config_done |= ADV_CONFIG_FLAG;
        ret = esp_ble_gap_config_adv_data(&scan_rsp_data);
        if (ret) ESP_LOGE(BLE_TAG, "config scan rsp failed: %x", ret);
        adv_config_done |= SCAN_RSP_CONFIG_FLAG;
        ret = esp_ble_gatts_create_attr_tab(gatt_db, gatts_if, DEVICE_IDX_NB, SVC_INST_ID);
        if (ret) ESP_LOGE(BLE_TAG, "create attr table failed: %x", ret);
        break;
    }
    case ESP_GATTS_READ_EVT:
        ESP_LOGI(BLE_TAG, "ESP_GATTS_READ_EVT");
        handle_read_event(gatts_if, param);
        break;

    case ESP_GATTS_WRITE_EVT:
        if (!param->write.is_prep) {
            handle_write_event(gatts_if, param);
            if (param->write.need_rsp) {
                esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                            param->write.trans_id, ESP_GATT_OK, NULL);
            }
        }
        break;

    case ESP_GATTS_EXEC_WRITE_EVT:
        ESP_LOGI(BLE_TAG, "ESP_GATTS_EXEC_WRITE_EVT");
        break;

    case ESP_GATTS_MTU_EVT:
        ESP_LOGI(BLE_TAG, "MTU %d", param->mtu.mtu);
        break;

    case ESP_GATTS_CONF_EVT:
        ESP_LOGI(BLE_TAG, "CONF status=%d handle=%d", param->conf.status, param->conf.handle);
        break;

    case ESP_GATTS_START_EVT:
        ESP_LOGI(BLE_TAG, "SERVICE_START status=%d handle=%d",
                 param->start.status, param->start.service_handle);
        break;

    case ESP_GATTS_CONNECT_EVT:
        ESP_LOGI(BLE_TAG, "CONNECT conn_id=%d", param->connect.conn_id);
        ble_gatts_if    = gatts_if;
        ble_conn_id     = param->connect.conn_id;
        ble_is_connected = true;
        ESP_LOG_BUFFER_HEX(BLE_TAG, param->connect.remote_bda, 6);
        esp_ble_conn_update_params_t conn_params = {0};
        memcpy(conn_params.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        conn_params.latency = 0;
        conn_params.max_int = 0x20;
        conn_params.min_int = 0x10;
        conn_params.timeout = 400;
        esp_ble_gap_update_conn_params(&conn_params);
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        ESP_LOGI(BLE_TAG, "DISCONNECT reason=0x%x", param->disconnect.reason);
        ble_is_connected  = false;
        ble_cmd_leer      = false;
        esp_ble_gap_start_advertising(&adv_params);
        break;

    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        if (param->add_attr_tab.status != ESP_GATT_OK) {
            ESP_LOGE(BLE_TAG, "create attr table failed, error=0x%x",
                     param->add_attr_tab.status);
        } else if (param->add_attr_tab.num_handle != DEVICE_IDX_NB) {
            ESP_LOGE(BLE_TAG, "attr table handle count mismatch: got %d, expected %d",
                     param->add_attr_tab.num_handle, DEVICE_IDX_NB);
        } else {
            ESP_LOGI(BLE_TAG, "Attr table OK, handles=%d", param->add_attr_tab.num_handle);
            memcpy(device_handle_table, param->add_attr_tab.handles,
                   sizeof(device_handle_table));
            esp_ble_gatts_start_service(device_handle_table[DEVICE_SVC]);
        }
        break;

    case ESP_GATTS_STOP_EVT:
    case ESP_GATTS_OPEN_EVT:
    case ESP_GATTS_CANCEL_OPEN_EVT:
    case ESP_GATTS_CLOSE_EVT:
    case ESP_GATTS_LISTEN_EVT:
    case ESP_GATTS_CONGEST_EVT:
    case ESP_GATTS_UNREG_EVT:
    case ESP_GATTS_DELETE_EVT:
    default:
        break;
    }
}

// ── GATT event dispatcher ─────────────────────────────────────────────────────
static void gatts_event_handler(esp_gatts_cb_event_t event,
                                esp_gatt_if_t gatts_if,
                                esp_ble_gatts_cb_param_t *param)
{
    if (event == ESP_GATTS_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) {
            device_profile_tab[PROFILE_APP_IDX].gatts_if = gatts_if;
        } else {
            ESP_LOGE(BLE_TAG, "reg app failed, app_id %04x, status %d",
                     param->reg.app_id, param->reg.status);
            return;
        }
    }
    for (int idx = 0; idx < PROFILE_NUM; idx++) {
        if (gatts_if == ESP_GATT_IF_NONE ||
            gatts_if == device_profile_tab[idx].gatts_if) {
            if (device_profile_tab[idx].gatts_cb) {
                device_profile_tab[idx].gatts_cb(event, gatts_if, param);
            }
        }
    }
}

// ── API pública ───────────────────────────────────────────────────────────────
void ble_init() {
    esp_err_t ret;

    esp_bluedroid_status_t ble_status = esp_bluedroid_get_status();
    if (ble_status == ESP_BLUEDROID_STATUS_ENABLED) {
        ESP_LOGI(BLE_TAG, "BLE ya estaba habilitado");
        return;
    }

    ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
        ESP_LOGW(BLE_TAG, "Memory release: %s", esp_err_to_name(ret));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));

    esp_bluedroid_config_t bld_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bld_cfg));
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_event_handler));
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gatts_app_register(ESP_APP_ID));
    ESP_ERROR_CHECK(esp_ble_gatt_set_local_mtu(500));
}

void ble_enviar_status(const char* mensaje) {
    if (!ble_is_connected) {
        ESP_LOGW(BLE_TAG, "Sin cliente BLE — notificación descartada: %s", mensaje);
        return;
    }
    uint16_t attr_handle = device_handle_table[DEVICE_CHAR_VAL_STATUS];
    esp_ble_gatts_send_indicate(
        ble_gatts_if, ble_conn_id, attr_handle,
        strlen(mensaje), (uint8_t *)mensaje,
        false);
    ESP_LOGI(BLE_TAG, "Notificación → %s", mensaje);
}

void ble_deinit() {
    esp_bluedroid_status_t ble_status = esp_bluedroid_get_status();
    if (ble_status == ESP_BLUEDROID_STATUS_UNINITIALIZED ||
        ble_status == ESP_BLUEDROID_STATUS_INITIALIZED) {
        ESP_LOGI(BLE_TAG, "BLE no estaba activo");
        return;
    }
    ESP_ERROR_CHECK(esp_bluedroid_disable());
    ESP_ERROR_CHECK(esp_bluedroid_deinit());
    ESP_ERROR_CHECK(esp_bt_controller_disable());
    ESP_ERROR_CHECK(esp_bt_controller_deinit());
}
