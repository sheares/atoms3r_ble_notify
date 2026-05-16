#include "ble_nus.h"
#include <string.h>
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble_nus";

static ble_cmd_cb_t s_cmd_cb       = NULL;
static bool         s_connected    = false;
static uint16_t     s_tx_handle    = 0;
static char         s_device_name[32];

// ─── NUS UUIDs (128-bit, little-endian) ──────────────────────────────────────
// Service:  6E400001-B5A3-F393-E0A9-E50E24DCCA9E
// RX (write): 6E400002-...
// TX (notify): 6E400003-...

static const ble_uuid128_t nus_svc_uuid = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,
    0x93,0xf3,0xa3,0xb5,0x01,0x00,0x40,0x6e
);
static const ble_uuid128_t nus_rx_uuid = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,
    0x93,0xf3,0xa3,0xb5,0x02,0x00,0x40,0x6e
);
static const ble_uuid128_t nus_tx_uuid = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,
    0x93,0xf3,0xa3,0xb5,0x03,0x00,0x40,0x6e
);

// ─── GATT callbacks ───────────────────────────────────────────────────────────

static int nus_rx_write_cb(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;

    char buf[32] = {0};
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL);

    ESP_LOGI(TAG, "rx: %s", buf);

    if (!s_cmd_cb) return 0;

    if      (strncmp(buf, "thinking", 8) == 0) s_cmd_cb(BLE_CMD_THINKING, NULL);
    else if (strncmp(buf, "waiting",  7) == 0) s_cmd_cb(BLE_CMD_WAITING,  NULL);
    else if (strncmp(buf, "done",     4) == 0) s_cmd_cb(BLE_CMD_DONE,     NULL);
    else if (strncmp(buf, "standby",  7) == 0) s_cmd_cb(BLE_CMD_STANDBY,  NULL);
    else if (strncmp(buf, "clear",    5) == 0) s_cmd_cb(BLE_CMD_STANDBY,  NULL);
    else if (strncmp(buf, "dizzy",    5) == 0) s_cmd_cb(BLE_CMD_DIZZY,    NULL);
    else if (strncmp(buf, "theme ",   6) == 0) s_cmd_cb(BLE_CMD_THEME,    buf + 6);
    else if (strncmp(buf, "tool ",    5) == 0) s_cmd_cb(BLE_CMD_TOOL,     buf + 5);
    else if (strncmp(buf, "tool",     4) == 0) s_cmd_cb(BLE_CMD_TOOL,     NULL);
    else if (strncmp(buf, "time ",    5) == 0) s_cmd_cb(BLE_CMD_TIME,     buf + 5);
    else if (strncmp(buf, "sessions ", 9) == 0) s_cmd_cb(BLE_CMD_SESSIONS, buf + 9);

    return 0;
}

static int nus_tx_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return 0;
}

static const struct ble_gatt_svc_def s_nus_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &nus_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid       = &nus_rx_uuid.u,
                .access_cb  = nus_rx_write_cb,
                .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid       = &nus_tx_uuid.u,
                .access_cb  = nus_tx_access_cb,
                .val_handle = &s_tx_handle,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 },
        },
    },
    { 0 },
};

// ─── GAP ──────────────────────────────────────────────────────────────────────

static void ble_advertise(void);

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_connected = true;
            ESP_LOGI(TAG, "connected");
        } else {
            s_connected = false;
            ble_advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_connected = false;
        ESP_LOGI(TAG, "disconnected, reason=%d", event->disconnect.reason);
        ble_advertise();
        break;
    default:
        break;
    }
    return 0;
}

static void ble_advertise(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags            = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name             = (const uint8_t *)s_device_name;
    fields.name_len         = strlen(s_device_name);
    fields.name_is_complete = 1;
    ble_gap_adv_set_fields(&fields);

    struct ble_gap_adv_params adv = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv, ble_gap_event, NULL);
    ESP_LOGI(TAG, "advertising as '%s'", s_device_name);
}

static void ble_on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_advertise();
}

static void nimble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ─── Public API ───────────────────────────────────────────────────────────────

bool ble_nus_init(const char *device_name, ble_cmd_cb_t cb)
{
    s_cmd_cb = cb;
    strncpy(s_device_name, device_name, sizeof(s_device_name) - 1);

    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed");
        return false;
    }

    ble_hs_cfg.sync_cb = ble_on_sync;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(s_nus_svcs);
    ble_gatts_add_svcs(s_nus_svcs);
    ble_svc_gap_device_name_set(device_name);

    nimble_port_freertos_init(nimble_host_task);
    return true;
}

bool ble_nus_connected(void)
{
    return s_connected;
}
