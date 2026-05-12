#include "gc9107.h"
#include "font.h"

#include <string.h>
#include <stdlib.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "gc9107";

static spi_device_handle_t s_spi;

// Full-frame buffer (big-endian RGB565).
// All draw functions write here; gc9107_flush() sends it to the display in one shot.
static uint8_t s_fb[LCD_WIDTH * LCD_HEIGHT * 2];

// Rotation state: 0=normal, 1=90°CW, 2=180°, 3=270°CW
static uint8_t s_rotation = 3;
static uint8_t s_fb_tx[LCD_WIDTH * LCD_HEIGHT * 2];  // scratch for rotated output

// ─── Init command table ───────────────────────────────────────────────────────
// Format: { cmd, {data bytes...}, num_data_bytes }
// Special num_data_bytes values:
//   0x80 | N  → send N data bytes then delay 120 ms  (used for SLPOUT)
//   0x81 | N  → send N data bytes then delay 20 ms   (used for DISPON)
//   0xFF      → end of table

typedef struct {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t len;
} lcd_init_cmd_t;

static const lcd_init_cmd_t s_init_cmds[] = {
    {0xEF, {0x00},                                                          0},
    {0xEB, {0x14},                                                          1},
    {0xFE, {0x00},                                                          0},
    {0xEF, {0x00},                                                          0},
    {0xEB, {0x14},                                                          1},
    {0x84, {0x40},                                                          1},
    {0x85, {0xFF},                                                          1},
    {0x86, {0xFF},                                                          1},
    {0x87, {0xFF},                                                          1},
    {0x88, {0x0A},                                                          1},
    {0x89, {0x21},                                                          1},
    {0x8A, {0x00},                                                          1},
    {0x8B, {0x80},                                                          1},
    {0x8C, {0x01},                                                          1},
    {0x8D, {0x01},                                                          1},
    {0x8E, {0xFF},                                                          1},
    {0x8F, {0xFF},                                                          1},
    {0xB6, {0x00, 0x20},                                                    2},
    {0x36, {0x00},                                                          1},
    {0x3A, {0x05},                                                          1},
    {0x90, {0x08, 0x08, 0x08, 0x08},                                        4},
    {0xBD, {0x06},                                                          1},
    {0xBC, {0x00},                                                          1},
    {0xFF, {0x60, 0x01, 0x04},                                              3},
    {0xC3, {0x13},                                                          1},
    {0xC4, {0x13},                                                          1},
    {0xC9, {0x22},                                                          1},
    {0xBE, {0x11},                                                          1},
    {0xE1, {0x10, 0x0E},                                                    2},
    {0xDF, {0x21, 0x0C, 0x02},                                              3},
    {0xF0, {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A},                           6},
    {0xF1, {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F},                           6},
    {0xF2, {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A},                           6},
    {0xF3, {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F},                           6},
    {0xED, {0x1B, 0x0B},                                                    2},
    {0xAE, {0x77},                                                          1},
    {0xCD, {0x63},                                                          1},
    {0x70, {0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03},        9},
    {0xE8, {0x34},                                                          1},
    {0x62, {0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70}, 12},
    {0x63, {0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70}, 12},
    {0x64, {0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07},                     7},
    {0x66, {0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00},  10},
    {0x67, {0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98},  10},
    {0x74, {0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00},                     7},
    {0x98, {0x3E, 0x07},                                                    2},
    {0x35, {0x00},                                                          0},
    {0x21, {0x00},                                                          0},
    {0x11, {0x00},                                               0x80 | 0},
    {0x29, {0x00},                                               0x81 | 0},
    {0x00, {0x00},                                                       0xFF},
};

// ─── Low-level SPI helpers ────────────────────────────────────────────────────
static inline void lcd_dc_cmd(void)  { gpio_set_level(LCD_PIN_DC, 0); }
static inline void lcd_dc_data(void) { gpio_set_level(LCD_PIN_DC, 1); }

static void lcd_write_byte(uint8_t byte, bool is_cmd)
{
    if (is_cmd) lcd_dc_cmd(); else lcd_dc_data();
    spi_transaction_t t = {
        .length    = 8,
        .tx_buffer = &byte,
        .flags     = 0,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

static void lcd_write_bytes(const uint8_t *buf, size_t len)
{
    if (len == 0) return;
    lcd_dc_data();
    spi_transaction_t t = {
        .length    = len * 8,
        .tx_buffer = buf,
        .flags     = 0,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

// ─── Window / pixel streaming ─────────────────────────────────────────────────
static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    x0 += LCD_COL_OFFSET;  x1 += LCD_COL_OFFSET;
    y0 += LCD_ROW_OFFSET;  y1 += LCD_ROW_OFFSET;

    uint8_t col_buf[4] = { x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF };
    uint8_t row_buf[4] = { y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF };

    lcd_write_byte(0x2A, true);
    lcd_write_bytes(col_buf, 4);
    lcd_write_byte(0x2B, true);
    lcd_write_bytes(row_buf, 4);
    lcd_write_byte(0x2C, true);
}

// ─── Framebuffer helpers ──────────────────────────────────────────────────────
static inline void fb_pixel(int16_t x, int16_t y, uint16_t color)
{
    if ((uint16_t)x >= LCD_WIDTH || (uint16_t)y >= LCD_HEIGHT) return;
    int idx = (y * LCD_WIDTH + x) * 2;
    s_fb[idx]     = color >> 8;
    s_fb[idx + 1] = color & 0xFF;
}

// ─── Public API ───────────────────────────────────────────────────────────────
void gc9107_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LCD_PIN_DC) | (1ULL << LCD_PIN_RST),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    gpio_set_level(LCD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LCD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LCD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = LCD_PIN_MOSI,
        .miso_io_num     = -1,
        .sclk_io_num     = LCD_PIN_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * 2 + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 40 * 1000 * 1000,
        .mode           = 0,
        .spics_io_num   = LCD_PIN_CS,
        .queue_size     = 1,
        .flags          = SPI_DEVICE_NO_DUMMY,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev_cfg, &s_spi));

    for (int i = 0; s_init_cmds[i].len != 0xFF; i++) {
        const lcd_init_cmd_t *c = &s_init_cmds[i];
        uint8_t n = c->len & 0x7F;

        lcd_write_byte(c->cmd, true);
        if (n > 0) lcd_write_bytes(c->data, n);

        if      (c->len == (0x80 | 0)) vTaskDelay(pdMS_TO_TICKS(120));
        else if (c->len == (0x81 | 0)) vTaskDelay(pdMS_TO_TICKS(20));
    }

    memset(s_fb, 0, sizeof(s_fb));
    gc9107_flush();
    ESP_LOGI(TAG, "GC9107 initialised (%dx%d)", LCD_WIDTH, LCD_HEIGHT);
}

void gc9107_set_rotation(uint8_t rot)
{
    s_rotation = rot & 3;
}

void gc9107_flush(void)
{
    lcd_set_window(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    if (s_rotation == 0) {
        lcd_write_bytes(s_fb, sizeof(s_fb));
        return;
    }
    for (int y = 0; y < LCD_HEIGHT; y++) {
        for (int x = 0; x < LCD_WIDTH; x++) {
            int sx, sy;
            switch (s_rotation) {
            case 1: sx = y;                sy = LCD_HEIGHT - 1 - x; break; // 90° CW
            case 2: sx = LCD_WIDTH  - 1 - x; sy = LCD_HEIGHT - 1 - y; break; // 180°
            case 3: sx = LCD_WIDTH  - 1 - y; sy = x;                  break; // 270° CW
            default: sx = x; sy = y; break;
            }
            int si = (sy * LCD_WIDTH + sx) * 2;
            int di = (y  * LCD_WIDTH + x ) * 2;
            s_fb_tx[di]     = s_fb[si];
            s_fb_tx[di + 1] = s_fb[si + 1];
        }
    }
    lcd_write_bytes(s_fb_tx, sizeof(s_fb_tx));
}

void gc9107_fill_screen(uint16_t color)
{
    uint8_t hi = color >> 8, lo = color & 0xFF;
    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) {
        s_fb[i * 2]     = hi;
        s_fb[i * 2 + 1] = lo;
    }
}

void gc9107_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_WIDTH)  w = LCD_WIDTH  - x;
    if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;

    uint8_t hi = color >> 8, lo = color & 0xFF;
    for (int16_t row = y; row < y + h; row++) {
        uint8_t *p = &s_fb[(row * LCD_WIDTH + x) * 2];
        for (int16_t col = 0; col < w; col++) {
            *p++ = hi;
            *p++ = lo;
        }
    }
}

void gc9107_draw_pixel(int16_t x, int16_t y, uint16_t color)
{
    fb_pixel(x, y, color);
}

void gc9107_draw_hline(int16_t x, int16_t y, int16_t len, uint16_t color)
{
    gc9107_fill_rect(x, y, len, 1, color);
}

void gc9107_draw_vline(int16_t x, int16_t y, int16_t len, uint16_t color)
{
    gc9107_fill_rect(x, y, 1, len, color);
}

void gc9107_draw_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    gc9107_draw_hline(x,         y,         w, color);
    gc9107_draw_hline(x,         y + h - 1, w, color);
    gc9107_draw_vline(x,         y,         h, color);
    gc9107_draw_vline(x + w - 1, y,         h, color);
}

void gc9107_draw_circle(int16_t cx, int16_t cy, int16_t r, uint16_t color)
{
    int16_t f  = 1 - r, ddf_x = 1, ddf_y = -2 * r, x = 0, y = r;
    fb_pixel(cx,     cy + r, color);
    fb_pixel(cx,     cy - r, color);
    fb_pixel(cx + r, cy,     color);
    fb_pixel(cx - r, cy,     color);
    while (x < y) {
        if (f >= 0) { y--; ddf_y += 2; f += ddf_y; }
        x++; ddf_x += 2; f += ddf_x;
        fb_pixel(cx + x, cy + y, color);
        fb_pixel(cx - x, cy + y, color);
        fb_pixel(cx + x, cy - y, color);
        fb_pixel(cx - x, cy - y, color);
        fb_pixel(cx + y, cy + x, color);
        fb_pixel(cx - y, cy + x, color);
        fb_pixel(cx + y, cy - x, color);
        fb_pixel(cx - y, cy - x, color);
    }
}

void gc9107_fill_circle(int16_t cx, int16_t cy, int16_t r, uint16_t color)
{
    gc9107_draw_hline(cx - r, cy, 2 * r + 1, color);
    int16_t f  = 1 - r, ddf_x = 1, ddf_y = -2 * r, x = 0, y = r;
    while (x < y) {
        if (f >= 0) { y--; ddf_y += 2; f += ddf_y; }
        x++; ddf_x += 2; f += ddf_x;
        gc9107_draw_hline(cx - x, cy + y, 2 * x + 1, color);
        gc9107_draw_hline(cx - x, cy - y, 2 * x + 1, color);
        gc9107_draw_hline(cx - y, cy + x, 2 * y + 1, color);
        gc9107_draw_hline(cx - y, cy - x, 2 * y + 1, color);
    }
}

void gc9107_draw_char(int16_t x, int16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale)
{
    if (c < FONT_FIRST || c > FONT_LAST) c = '?';
    const uint8_t *glyph = font5x8[c - FONT_FIRST];

    for (uint8_t col = 0; col < FONT_WIDTH; col++) {
        uint8_t col_data = glyph[col];
        for (uint8_t row = 0; row < FONT_HEIGHT; row++) {
            uint16_t color = (col_data & (1 << row)) ? fg : bg;
            if (scale == 1) {
                fb_pixel(x + col, y + row, color);
            } else {
                gc9107_fill_rect(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
    gc9107_fill_rect(x + FONT_WIDTH * scale, y, scale, FONT_HEIGHT * scale, bg);
}

void gc9107_draw_string(int16_t x, int16_t y, const char *str, uint16_t fg, uint16_t bg, uint8_t scale)
{
    while (*str) {
        gc9107_draw_char(x, y, *str++, fg, bg, scale);
        x += (FONT_WIDTH + 1) * scale;
        if (x + FONT_WIDTH * scale > LCD_WIDTH) break;
    }
}
