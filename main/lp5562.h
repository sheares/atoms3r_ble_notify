#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c_master.h"

// ─── AtomS3R system I2C bus (LP5562 + BMI270 share this bus) ─────────────────
#define SYS_I2C_SDA     45
#define SYS_I2C_SCL      0
#define SYS_I2C_PORT    I2C_NUM_0
#define SYS_I2C_FREQ    400000

// ─── LP5562 LED driver (RGB LED + LCD backlight W-channel) ───────────────────
#define LP5562_ADDR     0x30  // AD0=AD1=GND

// ─── Public API ───────────────────────────────────────────────────────────────
// Creates the shared I2C bus and initialises LP5562.
// Returns the bus handle (needed by bmi270_init).
bool lp5562_init(i2c_master_bus_handle_t *out_bus);

// Set LCD backlight brightness 0–255 (255 = full on).
bool lp5562_set_backlight(i2c_master_bus_handle_t bus, uint8_t brightness);
