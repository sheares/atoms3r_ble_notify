#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c_master.h"

// ─── AtomS3R internal IMU (BMI270) ───────────────────────────────────────────
// BMI270 shares the SYS I2C bus with the LP5562 LED driver
// Bus pins defined in lp5562.h (SDA=GPIO45, SCL=GPIO0)
#define BMI270_ADDR     0x68
#define BMI270_I2C_FREQ 400000

// ─── Public API ───────────────────────────────────────────────────────────────
// Accepts the already-initialised SYS I2C bus handle.
// Blocks ~300 ms during firmware config upload.
bool bmi270_init(i2c_master_bus_handle_t bus);

// Read raw signed 16-bit accelerometer values.
// At ±2 g range: 1 g = 16384 LSB.
bool bmi270_read_accel(int16_t *ax, int16_t *ay, int16_t *az);
