#pragma once
#include <stdbool.h>

typedef enum {
    BLE_CMD_THINKING,
    BLE_CMD_WAITING,
    BLE_CMD_DONE,
    BLE_CMD_STANDBY,
    BLE_CMD_DIZZY,
    BLE_CMD_THEME,
} ble_cmd_t;

typedef void (*ble_cmd_cb_t)(ble_cmd_t cmd, const char *arg);

bool ble_nus_init(const char *device_name, ble_cmd_cb_t cb);
bool ble_nus_connected(void);
