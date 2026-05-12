#include "bmi270.h"
#include "bmi270_config.h"

#include <string.h>
#include <stdlib.h>
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bmi270";

// ─── BMI270 register addresses ────────────────────────────────────────────────
#define REG_CHIP_ID         0x00
#define REG_ACC_X_LSB       0x0C
#define REG_INTERNAL_STATUS 0x21
#define REG_ACC_CONF        0x40
#define REG_ACC_RANGE       0x41
#define REG_INIT_CTRL       0x59
#define REG_INIT_ADDR_0     0x5B
#define REG_INIT_ADDR_1     0x5C
#define REG_INIT_DATA       0x5E
#define REG_PWR_CONF        0x7C
#define REG_PWR_CTRL        0x7D
#define REG_CMD             0x7E

#define BMI270_CHIP_ID      0x24
#define CONFIG_CHUNK        512     // bytes per config burst
#define CONFIG_SIZE         (sizeof(bmi270_config_file))

// ─── Internal state ───────────────────────────────────────────────────────────
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;

// ─── Low-level I2C helpers ────────────────────────────────────────────────────
static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev, buf, 2, pdMS_TO_TICKS(100));
}

static esp_err_t read_reg(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1,
                                       pdMS_TO_TICKS(100));
}

static esp_err_t read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len,
                                       pdMS_TO_TICKS(100));
}

static esp_err_t write_data_chunk(const uint8_t *data, size_t len)
{
    uint8_t *buf = malloc(len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    buf[0] = REG_INIT_DATA;
    memcpy(buf + 1, data, len);
    esp_err_t ret = i2c_master_transmit(s_dev, buf, len + 1, pdMS_TO_TICKS(2000));
    free(buf);
    return ret;
}

// ─── Config file upload ───────────────────────────────────────────────────────
static bool upload_config(void)
{
    esp_err_t ret;

    ret = write_reg(REG_INIT_CTRL, 0x00);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "INIT_CTRL=0 failed: 0x%x", ret);
        return false;
    }

    for (size_t i = 0; i < CONFIG_SIZE; i += CONFIG_CHUNK) {
        size_t chunk = (CONFIG_SIZE - i < CONFIG_CHUNK) ? CONFIG_SIZE - i : CONFIG_CHUNK;

        uint16_t word_addr = (uint16_t)(i / 2);
        uint8_t addr_lsb   = (uint8_t)(word_addr & 0x0F);
        uint8_t addr_msb   = (uint8_t)(word_addr >> 4);

        ret = write_reg(REG_INIT_ADDR_0, addr_lsb);
        if (ret != ESP_OK) { ESP_LOGE(TAG, "ADDR_0 err at %u", (unsigned)i); return false; }
        ret = write_reg(REG_INIT_ADDR_1, addr_msb);
        if (ret != ESP_OK) { ESP_LOGE(TAG, "ADDR_1 err at %u", (unsigned)i); return false; }

        ret = write_data_chunk(&bmi270_config_file[i], chunk);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Data chunk failed at byte %u: 0x%x", (unsigned)i, ret);
            return false;
        }
    }

    ret = write_reg(REG_INIT_CTRL, 0x01);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "INIT_CTRL=1 failed: 0x%x", ret);
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(150));

    uint8_t status = 0;
    read_reg(REG_INTERNAL_STATUS, &status);
    if ((status & 0x0F) != 0x01) {
        ESP_LOGE(TAG, "Config load failed: INTERNAL_STATUS=0x%02X", status);
        return false;
    }
    ESP_LOGI(TAG, "Config loaded OK (%u bytes)", (unsigned)CONFIG_SIZE);
    return true;
}

// ─── Public API ───────────────────────────────────────────────────────────────
bool bmi270_init(i2c_master_bus_handle_t bus)
{
    esp_err_t ret;
    s_bus = bus;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BMI270_ADDR,
        .scl_speed_hz    = BMI270_I2C_FREQ,
    };
    ret = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C add device failed: 0x%x", ret);
        return false;
    }

    // Soft reset — ignore error (device may not ACK during reset)
    write_reg(REG_CMD, 0xB6);
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t chip_id = 0;
    ret = read_reg(REG_CHIP_ID, &chip_id);
    ESP_LOGI(TAG, "CHIP_ID=0x%02X (expect 0x%02X, read_ret=0x%x)", chip_id, BMI270_CHIP_ID, ret);
    if (ret != ESP_OK || chip_id != BMI270_CHIP_ID) {
        ESP_LOGE(TAG, "BMI270 not found (SDA=45 SCL=0 addr=0x%02X)", BMI270_ADDR);
        return false;
    }

    ret = write_reg(REG_PWR_CONF, 0x00);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "PWR_CONF err: 0x%x", ret); return false; }
    vTaskDelay(pdMS_TO_TICKS(1));

    if (!upload_config()) return false;

    ret = write_reg(REG_PWR_CTRL, 0x04);  // enable accelerometer
    if (ret != ESP_OK) { ESP_LOGE(TAG, "PWR_CTRL err: 0x%x", ret); return false; }
    vTaskDelay(pdMS_TO_TICKS(22));

    ret = write_reg(REG_ACC_CONF, 0xA8);  // 100Hz, normal BWP, perf mode
    if (ret != ESP_OK) { ESP_LOGE(TAG, "ACC_CONF err: 0x%x", ret); return false; }
    ret = write_reg(REG_ACC_RANGE, 0x00); // ±2g
    if (ret != ESP_OK) { ESP_LOGE(TAG, "ACC_RANGE err: 0x%x", ret); return false; }
    vTaskDelay(pdMS_TO_TICKS(5));

    ESP_LOGI(TAG, "BMI270 ready");
    return true;
}

bool bmi270_read_accel(int16_t *ax, int16_t *ay, int16_t *az)
{
    uint8_t buf[6];
    esp_err_t ret = read_regs(REG_ACC_X_LSB, buf, 6);
    if (ret != ESP_OK) return false;

    *ax = (int16_t)(buf[0] | ((uint16_t)buf[1] << 8));
    *ay = (int16_t)(buf[2] | ((uint16_t)buf[3] << 8));
    *az = (int16_t)(buf[4] | ((uint16_t)buf[5] << 8));
    return true;
}
