#include "lp5562.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lp5562";

// ─── LP5562 register map ──────────────────────────────────────────────────────
#define REG_ENABLE      0x00  // bit6=CHIP_EN, bit5=LOG_EN
#define REG_OP_MODE     0x01  // bits[7:6]=W, [5:4]=R, [3:2]=G, [1:0]=B (11=direct)
#define REG_B_PWM       0x02
#define REG_G_PWM       0x03
#define REG_R_PWM       0x04
#define REG_CONFIG      0x08  // bit1=INT_CLK_EN
#define REG_W_PWM       0x0E  // white (backlight) PWM 0–255
#define REG_W_CURRENT   0x0F  // white channel current (0.1 mA/step)

static i2c_master_dev_handle_t s_dev;

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev, buf, 2, pdMS_TO_TICKS(100));
}

bool lp5562_init(i2c_master_bus_handle_t *out_bus)
{
    // ── Create shared system I2C bus ─────────────────────────────────────────
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port                     = SYS_I2C_PORT,
        .sda_io_num                   = SYS_I2C_SDA,
        .scl_io_num                   = SYS_I2C_SCL,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: 0x%x", ret);
        return false;
    }
    *out_bus = bus;

    // ── Add LP5562 device ────────────────────────────────────────────────────
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = LP5562_ADDR,
        .scl_speed_hz    = SYS_I2C_FREQ,
    };
    ret = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LP5562 add device failed: 0x%x", ret);
        return false;
    }

    // ── Initialise LP5562 ────────────────────────────────────────────────────
    // Enable chip
    ret = write_reg(REG_ENABLE, 0x40);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LP5562 not responding at 0x%02X (err 0x%x)", LP5562_ADDR, ret);
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(1));

    // Use internal oscillator
    write_reg(REG_CONFIG, 0x01);

    // Set W channel current to 10 mA (safe for the display backlight)
    write_reg(REG_W_CURRENT, 0x64);

    // W channel off initially
    write_reg(REG_W_PWM, 0x00);

    // Set W to direct-PWM mode (bits 7:6 = 11), leave R/G/B disabled
    write_reg(REG_OP_MODE, 0xC0);

    ESP_LOGI(TAG, "LP5562 ready");
    return true;
}

bool lp5562_set_backlight(i2c_master_bus_handle_t bus, uint8_t brightness)
{
    (void)bus;  // s_dev already bound; bus arg kept for API symmetry
    esp_err_t ret = write_reg(REG_W_PWM, brightness);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set_backlight failed: 0x%x", ret);
        return false;
    }
    return true;
}
