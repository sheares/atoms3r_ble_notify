#pragma once
#include <stdint.h>
#include <stdbool.h>

// ─── AtomS3R LCD pin assignments (GC9107, 128×128) ───────────────────────────
// Verified from AtomS3R Schematic.png (Sch_M5_AtomS3R_v0.4.1)
#define LCD_PIN_MOSI   21   // SPI_MOSI
#define LCD_PIN_SCLK   15   // SPI_SCK
#define LCD_PIN_CS     14   // DISP_CS
#define LCD_PIN_DC     42   // DISP_RS (data/command)
#define LCD_PIN_RST    48   // DISP_RST
// Backlight is driven by LP5562 LED driver over I2C — no direct GPIO

// ─── Panel geometry ──────────────────────────────────────────────────────────
#define LCD_WIDTH      128
#define LCD_HEIGHT     128
// Physical pixel offsets baked into the GC9107 on the AtomS3R module
#define LCD_COL_OFFSET  2
#define LCD_ROW_OFFSET  1

// ─── RGB565 colour helpers ────────────────────────────────────────────────────
#define RGB565(r, g, b)  ((uint16_t)(((b & 0xF8) << 8) | ((g & 0xFC) << 3) | (r >> 3)))

#define COLOR_BLACK      0x0000
#define COLOR_WHITE      0xFFFF
#define COLOR_RED        0xF800
#define COLOR_GREEN      0x07E0
#define COLOR_BLUE       0x001F
#define COLOR_YELLOW     0xFFE0
#define COLOR_CYAN       0x07FF
#define COLOR_MAGENTA    0xF81F
#define COLOR_ORANGE     0xFD20
#define COLOR_GRAY       0x7BEF
#define COLOR_DARK_GRAY  0x39E7
#define COLOR_DARK_GREEN 0x03E0

// ─── Public API ──────────────────────────────────────────────────────────────
void     gc9107_init(void);
// Push the full framebuffer to the display — call once per frame after drawing.
void     gc9107_flush(void);
// Set screen rotation: 0=normal, 1=90°CW, 2=180°, 3=270°CW.
void     gc9107_set_rotation(uint8_t rot);

void     gc9107_fill_screen(uint16_t color);
void     gc9107_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void     gc9107_draw_pixel(int16_t x, int16_t y, uint16_t color);
void     gc9107_draw_hline(int16_t x, int16_t y, int16_t len, uint16_t color);
void     gc9107_draw_vline(int16_t x, int16_t y, int16_t len, uint16_t color);
void     gc9107_draw_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void     gc9107_draw_circle(int16_t cx, int16_t cy, int16_t r, uint16_t color);
void     gc9107_fill_circle(int16_t cx, int16_t cy, int16_t r, uint16_t color);

// Text – scale=1 → 6×9 rendered size (5×8 glyph + 1px spacing)
void     gc9107_draw_char(int16_t x, int16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale);
void     gc9107_draw_string(int16_t x, int16_t y, const char *str, uint16_t fg, uint16_t bg, uint8_t scale);
