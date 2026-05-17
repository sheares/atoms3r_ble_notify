#include "gc9107.h"
#include "lp5562.h"
#include "bmi270.h"
#include "ble_nus.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

static const char *TAG = "notify";

#define BTN_GPIO     41
#define DEVICE_NAME  "AtomS3R-Notify"

// ─── App state ────────────────────────────────────────────────────────────────
typedef enum {
    APP_DISCONNECTED,
    APP_STANDBY,
    APP_THINKING,
    APP_DONE,
    APP_WAITING,
    APP_DIZZY,    // shake-triggered, temporary
    APP_SLEEPY,   // idle-triggered from standby
    APP_DISCO,    // konami-triggered, temporary
} app_state_t;

typedef enum {
    THEME_DEFAULT,
    THEME_WEEKEND,
    THEME_CNY,
    THEME_CHRISTMAS,
    THEME_NATIONAL_DAY,
    THEME_DEEPAVALI,
    THEME_NEW_YEAR,
} theme_t;
static volatile theme_t s_theme = THEME_DEFAULT;

typedef enum {
    TOOL_NONE,
    TOOL_EDIT,
    TOOL_READ,
    TOOL_WRITE,
    TOOL_BASH,
    TOOL_WEB,
    TOOL_GREP,
    TOOL_GLOB,
    TOOL_TASK,
    TOOL_OTHER,
} tool_t;
static volatile tool_t s_tool = TOOL_NONE;
static int64_t s_tool_until_us = 0;
#define TOOL_TTL_US  (60LL * 1000000LL)  // 60s failsafe expiry

// Time-of-day mood. Daemon pushes `time <hh>` on connect and hourly.
// Until then, default to MOOD_DEFAULT (current behavior unchanged).
typedef enum {
    MOOD_DEFAULT,  // 10-17 work hours: as-was
    MOOD_ALERT,    // 06-10 morning: bright, fast blinks
    MOOD_MELLOW,   // 17-22 evening: dim backlight
    MOOD_TIRED,    // 22-06 late night: half-lid, dimmer still
} mood_t;
static volatile mood_t s_mood = MOOD_DEFAULT;
static volatile int   s_hour = -1;  // -1 = unknown

// Number of active Claude Code sessions (capped at 4 for display).
// Daemon broadcasts when count changes; 1 or 0 = no dots drawn.
static volatile int s_sessions = 1;

// Per-session bar state — one char per slot: T=thinking W=waiting D=done .=idle
// Filled by BLE_CMD_BAR. s_bar_count is the number of segments to draw.
// Falls back to a single dim segment when no bar data has arrived yet.
static volatile char s_bar_states[4] = {'.','.','.','.'};
static volatile int  s_bar_count     = 0;

// Per-session labels — short (max 5 char) text drawn above each bar segment.
// Filled by BLE_CMD_LABELS (pipe-separated). Empty string = no label drawn.
#define LABEL_MAX 6
static char s_labels[4][LABEL_MAX] = {{0},{0},{0},{0}};

static volatile app_state_t s_app_state = APP_DISCONNECTED;
static app_state_t          s_prev_state = APP_STANDBY;
static volatile int64_t     s_notify_us = 0;
static int64_t              s_last_change_us = 0;
static int64_t              s_dizzy_until_us = 0;
static int64_t              s_disco_until_us = 0;
static bool                 s_imu_ok    = false;
static i2c_master_bus_handle_t s_sys_bus = NULL;
static uint8_t              s_current_rot = 3;

#define SLEEPY_IDLE_US   (5LL * 60LL * 1000000LL)  // 5 minutes
#define DIZZY_LEN_US     (2LL * 1000000LL)
#define DISCO_LEN_US     (10LL * 1000000LL)

// ─── Colors ───────────────────────────────────────────────────────────────────
#define C_ORANGE    RGB565(245, 110,  45)
#define C_EYE       RGB565( 20,  15,  10)
#define C_WHITE     RGB565(255, 255, 255)
#define C_DIM       RGB565(170,  65,  20)
#define C_FAINT     RGB565(235, 175, 150)
#define C_DONE_BG   RGB565( 30, 160,  30)
#define C_DONE_DIM  RGB565( 20, 110,  20)
#define C_CHECK     RGB565(255, 255, 255)
#define C_TICK      RGB565( 60, 220,  60)
// Grey palette (disconnected screen)
#define C_GREY_BG   RGB565(150, 150, 150)
#define C_GREY_EYE  RGB565( 50,  50,  50)
#define C_GREY_DIM  RGB565( 70,  70,  70)
#define C_GREY_LT   RGB565(215, 215, 215)
// Blue palette (waiting/question screen)
#define C_BLUE_BG   RGB565( 20,  80, 200)
#define C_BLUE_DIM  RGB565( 15,  55, 140)
#define C_BLUE_EYE  RGB565(  5,  20,  60)
#define C_BLUE_LT   RGB565(160, 195, 240)
// Dizzy (purple)
#define C_DIZZY_BG  RGB565(155,  70, 195)
#define C_DIZZY_LT  RGB565(245, 220, 250)
#define C_DIZZY_EYE RGB565( 35,  10,  50)
#define C_DIZZY_DIM RGB565(110,  40, 145)
// Sleepy (dark blue)
#define C_SLEEPY_BG  RGB565( 18,  28,  55)
#define C_SLEEPY_LT  RGB565(120, 145, 215)
#define C_SLEEPY_EYE RGB565(  5,  10,  20)
#define C_SLEEPY_DIM RGB565( 60,  75, 115)

// ─── Pixel art eye bitmaps ────────────────────────────────────────────────────
#define PS       5
#define EYE_COLS 4

static const uint8_t BMP_OPEN[4][4]       = { {1,1,1,1},{1,1,1,1},{1,1,1,1},{1,1,1,1} };
static const uint8_t BMP_HALF[2][4]       = { {1,1,1,1},{1,1,1,1} };
static const uint8_t BMP_BLINK[1][4]      = { {1,1,1,1} };
static const uint8_t BMP_HAPPY[3][4]      = { {0,1,1,0},{1,1,1,1},{1,0,0,1} };
static const uint8_t BMP_CHEVRON_R[5][4]  = { {1,1,0,0},{0,1,1,0},{0,0,1,1},{0,1,1,0},{1,1,0,0} };
static const uint8_t BMP_CHEVRON_L[5][4]  = { {0,0,1,1},{0,1,1,0},{1,1,0,0},{0,1,1,0},{0,0,1,1} };
// Spiral/swirl for dizzy
static const uint8_t BMP_SPIRAL_A[4][4]   = { {0,1,1,0},{1,0,0,1},{1,0,1,1},{0,1,1,0} };
static const uint8_t BMP_SPIRAL_B[4][4]   = { {0,1,1,0},{1,1,0,1},{1,0,0,1},{0,1,1,0} };
// Closed lid w/ down-curve for sleepy
static const uint8_t BMP_SLEEP[2][4]      = { {1,0,0,1},{0,1,1,0} };
// Star eyes for disco
static const uint8_t BMP_STAR[4][4]       = { {0,1,1,0},{1,1,1,1},{1,1,1,1},{0,1,1,0} };

typedef enum { EYE_OPEN, EYE_HALF, EYE_BLINK, EYE_HAPPY } eye_state_t;

#define EYE_L_X  24
#define EYE_R_X 104
#define EYE_Y    48

// ─── Blink / wander / thinking state ─────────────────────────────────────────
static int   s_blink_timer  = 90;
static int   s_blink_phase  = 0;
static float s_eye_ox       = 0.0f;
static float s_eye_target_x = 0.0f;
static int   s_wander_timer = 60;
static int   s_think_tick   = 0;
static float s_eye_oy       = 0.0f;

// Blink variants — 1/50 chance per blink
typedef enum { BLINK_NORMAL, BLINK_WINK_L, BLINK_WINK_R, BLINK_EYE_ROLL } blink_variant_t;
static blink_variant_t s_blink_variant = BLINK_NORMAL;

// ─── Shake detection state ───────────────────────────────────────────────────
#define SHAKE_HIST 8
static int     s_shake_hist[SHAKE_HIST] = {0};
static int     s_shake_idx = 0;
static int64_t s_last_shake_us = 0;

// ─── Konami button code: short-short-long-long-short ─────────────────────────
#define KONAMI_LEN 5
static const uint8_t KONAMI_PATTERN[KONAMI_LEN] = {0, 0, 1, 1, 0}; // 0=short,1=long
static uint8_t  s_konami_buf[KONAMI_LEN] = {9,9,9,9,9};
static int      s_konami_count = 0;
static int64_t  s_konami_last_us = 0;

// ─── Helpers ──────────────────────────────────────────────────────────────────

static void mark_event(void) { s_last_change_us = esp_timer_get_time(); }

static void enter_state(app_state_t s)
{
    s_app_state = s;
    mark_event();
}

static void enter_dizzy(void)
{
    if (s_app_state != APP_DIZZY && s_app_state != APP_DISCO) {
        s_prev_state = s_app_state;
    }
    // Auto-dismiss DONE → standby
    if (s_prev_state == APP_DONE) s_prev_state = APP_STANDBY;
    s_app_state = APP_DIZZY;
    s_dizzy_until_us = esp_timer_get_time() + DIZZY_LEN_US;
    mark_event();
}

static void enter_disco(void)
{
    if (s_app_state != APP_DIZZY && s_app_state != APP_DISCO) {
        s_prev_state = s_app_state;
    }
    s_app_state = APP_DISCO;
    s_disco_until_us = esp_timer_get_time() + DISCO_LEN_US;
    mark_event();
}

// ─── Drawing helpers ──────────────────────────────────────────────────────────

static void draw_centred(int y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale)
{
    int w = (int)strlen(s) * 6 * scale;
    gc9107_draw_string((LCD_WIDTH - w) / 2, y, s, fg, bg, scale);
}

static void draw_thick_line(int x0, int y0, int x1, int y1, int w, uint16_t color)
{
    int dx = abs(x1-x0), sx = x0<x1?1:-1;
    int dy = -abs(y1-y0), sy = y0<y1?1:-1;
    int err = dx+dy, half = w/2;
    while (1) {
        gc9107_fill_rect(x0-half, y0-half, w, w, color);
        if (x0==x1 && y0==y1) break;
        int e2 = 2*err;
        if (e2>=dy) { err+=dy; x0+=sx; }
        if (e2<=dx) { err+=dx; y0+=sy; }
    }
}

static void draw_pixel_eye(int cx, int cy, const uint8_t *bmp, int rows, uint16_t color)
{
    int ox = cx - (EYE_COLS*PS)/2;
    int oy = cy - (rows*PS)/2;
    for (int r=0; r<rows; r++)
        for (int c=0; c<EYE_COLS; c++)
            if (bmp[r*EYE_COLS+c])
                gc9107_fill_rect(ox+c*PS, oy+r*PS, PS, PS, color);
}

// Draw two eyes with potentially different states (supports wink)
static void draw_eyes_split(int lx, int rx, int y, eye_state_t left, eye_state_t right, uint16_t color)
{
    const uint8_t *bmpL; int rowsL;
    const uint8_t *bmpR; int rowsR;
    #define MAP_BMP(s, b, r) do { \
        switch (s) { \
        case EYE_OPEN:  b=&BMP_OPEN[0][0];  r=4; break; \
        case EYE_HALF:  b=&BMP_HALF[0][0];  r=2; break; \
        case EYE_BLINK: b=&BMP_BLINK[0][0]; r=1; break; \
        case EYE_HAPPY: b=&BMP_HAPPY[0][0]; r=3; break; \
        default:        b=&BMP_OPEN[0][0];  r=4; break; \
        } \
    } while (0)
    MAP_BMP(left, bmpL, rowsL);
    MAP_BMP(right, bmpR, rowsR);
    #undef MAP_BMP
    draw_pixel_eye(lx, y, bmpL, rowsL, color);
    draw_pixel_eye(rx, y, bmpR, rowsR, color);
}

static void draw_eyes(int lx, int rx, int y, eye_state_t state, uint16_t color)
{
    draw_eyes_split(lx, rx, y, state, state, color);
}

// ─── Animation updates ────────────────────────────────────────────────────────

static void update_blink(void)
{
    if (s_blink_phase == 0) {
        if (--s_blink_timer <= 0) {
            // Pick variant for this blink
            int roll = rand() % 50;
            if (roll == 0)      s_blink_variant = BLINK_WINK_L;
            else if (roll == 1) s_blink_variant = BLINK_WINK_R;
            else if (roll == 2) s_blink_variant = BLINK_EYE_ROLL;
            else                s_blink_variant = BLINK_NORMAL;
            s_blink_phase=1;
            s_blink_timer = (s_blink_variant == BLINK_EYE_ROLL) ? 8 : 2;
        }
    } else {
        if (--s_blink_timer <= 0) {
            if (++s_blink_phase > 3) {
                s_blink_phase=0;
                s_blink_timer=80+rand()%60;
                s_blink_variant = BLINK_NORMAL;
            } else {
                s_blink_timer = (s_blink_variant == BLINK_EYE_ROLL) ? 8 : 2;
            }
        }
    }
}

static eye_state_t blink_eye_state(void)
{
    switch (s_blink_phase) {
    case 1: return EYE_HALF;
    case 2: return EYE_BLINK;
    case 3: return EYE_HALF;
    default: return EYE_OPEN;
    }
}

static void update_wander(void)
{
    if (--s_wander_timer <= 0) {
        int choices[] = {-8,-4,0,0,0,4,8};
        s_eye_target_x = (float)choices[rand()%7];
        s_wander_timer = 40+rand()%80;
    }
    s_eye_ox += (s_eye_target_x - s_eye_ox) * 0.04f;
}

static void update_thinking_motion(void)
{
    s_think_tick++;
    if (--s_wander_timer <= 0) {
        int choices[] = {-11,-7,-4,4,7,11};
        s_eye_target_x = (float)choices[rand()%6];
        s_wander_timer = 8+rand()%14;
    }
    s_eye_ox += (s_eye_target_x - s_eye_ox) * 0.18f;
    s_eye_oy  = 3.0f * sinf(s_think_tick * 0.14f);
}

// ─── IMU helpers: shake ──────────────────────────────────────────────────────

// Sustained-shake detector: requires ~2s of motion to trigger.
// Energy accumulator: rises while motion is detected each frame, decays when still.
static int s_shake_energy = 0;
#define SHAKE_ENERGY_TRIGGER 70  // ~2s of vigorous shake at +2/frame, 30fps

static bool detect_shake(int16_t ax, int16_t ay, int16_t az)
{
    int mag = abs(ax) + abs(ay) + abs(az);
    s_shake_hist[s_shake_idx] = mag;
    s_shake_idx = (s_shake_idx + 1) % SHAKE_HIST;
    int mn = mag, mx = mag;
    for (int i = 0; i < SHAKE_HIST; i++) {
        if (s_shake_hist[i] < mn) mn = s_shake_hist[i];
        if (s_shake_hist[i] > mx) mx = s_shake_hist[i];
    }
    int range = mx - mn;

    if (range > 18000)      s_shake_energy += 2;
    else if (range > 10000) s_shake_energy += 1;
    else                    s_shake_energy -= 2;
    if (s_shake_energy < 0)   s_shake_energy = 0;
    if (s_shake_energy > 120) s_shake_energy = 120;

    int64_t now = esp_timer_get_time();
    if (s_shake_energy >= SHAKE_ENERGY_TRIGGER && now - s_last_shake_us > 2500000LL) {
        s_last_shake_us = now;
        s_shake_energy = 0;
        return true;
    }
    return false;
}

// ─── Theme overlay ────────────────────────────────────────────────────────────

static void draw_theme_overlay(void)
{
    static int s_th_tick = 0;
    s_th_tick++;

    int bx = LCD_WIDTH - 14;  // badge centre (top-right)
    int by = 22;

    switch (s_theme) {
    case THEME_WEEKEND: {
        uint16_t sun = RGB565(255, 215, 0);
        gc9107_fill_circle(bx, by, 6, sun);
        // 8 rays
        gc9107_fill_rect(bx-10, by-1,  3, 3, sun);
        gc9107_fill_rect(bx+8,  by-1,  3, 3, sun);
        gc9107_fill_rect(bx-1,  by-10, 3, 3, sun);
        gc9107_fill_rect(bx-1,  by+8,  3, 3, sun);
        gc9107_fill_rect(bx-8,  by-8,  2, 2, sun);
        gc9107_fill_rect(bx+7,  by-8,  2, 2, sun);
        gc9107_fill_rect(bx-8,  by+7,  2, 2, sun);
        gc9107_fill_rect(bx+7,  by+7,  2, 2, sun);
        break;
    }
    case THEME_CNY: {
        uint16_t red  = RGB565(220, 30, 30);
        uint16_t gold = RGB565(255, 200, 0);
        // Lantern body
        gc9107_fill_rect(bx-6, by-7, 13, 14, red);
        // Gold top & bottom caps
        gc9107_fill_rect(bx-7, by-9, 15, 3, gold);
        gc9107_fill_rect(bx-7, by+6, 15, 3, gold);
        // Tassel
        gc9107_draw_vline(bx, by+9, 3, gold);
        // Vertical gold strokes inside
        gc9107_draw_vline(bx-2, by-6, 12, gold);
        gc9107_draw_vline(bx+2, by-6, 12, gold);
        break;
    }
    case THEME_CHRISTMAS: {
        uint16_t green = RGB565(0, 200, 60);
        uint16_t brown = RGB565(140, 80, 20);
        uint16_t star  = RGB565(255, 230, 90);
        // Tree (7 rows, wider base)
        for (int r = 0; r < 7; r++) {
            int w = r + 1;
            gc9107_fill_rect(bx - w, by - 7 + r*2, 2*w + 1, 2, green);
        }
        // Trunk
        gc9107_fill_rect(bx-1, by+7, 3, 3, brown);
        // Star on top
        gc9107_draw_pixel(bx,   by-9, star);
        gc9107_draw_pixel(bx-1, by-8, star);
        gc9107_draw_pixel(bx+1, by-8, star);
        // Drifting snowflakes everywhere
        for (int i = 0; i < 5; i++) {
            int sx = (s_th_tick + i * 47) % LCD_WIDTH;
            int sy = ((s_th_tick / 2) + i * 31) % LCD_HEIGHT;
            gc9107_fill_rect(sx, sy, 2, 2, C_WHITE);
        }
        break;
    }
    case THEME_NATIONAL_DAY: {
        uint16_t red = RGB565(225, 30, 30);
        // Flag: red top, white bottom
        gc9107_fill_rect(bx-7, by-7, 15, 7, red);
        gc9107_fill_rect(bx-7, by,   15, 7, C_WHITE);
        // Crescent moon (red disc with white bite)
        gc9107_fill_circle(bx-3, by-3, 3, C_WHITE);
        gc9107_fill_circle(bx-1, by-3, 2, red);
        // Five tiny stars
        gc9107_draw_pixel(bx+2, by-5, C_WHITE);
        gc9107_draw_pixel(bx+4, by-3, C_WHITE);
        gc9107_draw_pixel(bx+5, by-1, C_WHITE);
        gc9107_draw_pixel(bx+4, by+1, C_WHITE);
        gc9107_draw_pixel(bx+2, by-1, C_WHITE);
        break;
    }
    case THEME_DEEPAVALI: {
        uint16_t flame    = RGB565(255, 165, 0);
        uint16_t flameHot = RGB565(255, 230, 90);
        uint16_t pot      = RGB565(130, 60, 20);
        // Pot (diya)
        gc9107_fill_rect(bx-7, by+3, 15, 4, pot);
        gc9107_draw_hline(bx-5, by+7, 11, RGB565(90, 40, 10));
        // Flame teardrop
        gc9107_fill_rect(bx-1, by-9, 3, 2, flame);
        gc9107_fill_rect(bx-2, by-7, 5, 3, flame);
        gc9107_fill_rect(bx-3, by-4, 7, 4, flame);
        gc9107_fill_rect(bx-1, by-6, 3, 4, flameHot);
        // Flickering sparks
        if ((s_th_tick / 6) & 1) {
            gc9107_fill_rect(bx-10, by-4, 2, 2, flame);
            gc9107_fill_rect(bx+9,  by-4, 2, 2, flame);
        } else {
            gc9107_draw_pixel(bx-9, by-7, flameHot);
            gc9107_draw_pixel(bx+9, by-7, flameHot);
        }
        break;
    }
    case THEME_NEW_YEAR: {
        uint16_t hues[] = {
            RGB565(255,  80,  80), RGB565(255, 220,  90),
            RGB565(120, 200, 255), RGB565(180, 100, 255),
            RGB565( 90, 230, 140),
        };
        // Big "2026" -ish star burst in corner
        uint16_t pop = hues[(s_th_tick / 4) % 5];
        gc9107_fill_rect(bx-1, by-7, 3, 3, pop);
        gc9107_fill_rect(bx-7, by-1, 3, 3, pop);
        gc9107_fill_rect(bx+5, by-1, 3, 3, pop);
        gc9107_fill_rect(bx-1, by+5, 3, 3, pop);
        gc9107_fill_rect(bx-1, by-1, 3, 3, hues[(s_th_tick / 6) % 5]);
        // Confetti everywhere
        for (int i = 0; i < 18; i++) {
            int sx = ((s_th_tick / 2) + i * 19) % LCD_WIDTH;
            int sy = (i * 31 + (s_th_tick / 3)) % LCD_HEIGHT;
            gc9107_fill_rect(sx, sy, 2, 2, hues[i % 5]);
        }
        break;
    }
    default:
        break;
    }
}

// ─── Tool icon overlay ────────────────────────────────────────────────────────
// Renders a small ~12x12 glyph in the top-right badge slot during thinking,
// replacing the theme badge while a tool is active.

static tool_t parse_tool_name(const char *s)
{
    if (!s || !*s) return TOOL_NONE;
    if (strncasecmp(s, "edit",    4) == 0) return TOOL_EDIT;
    if (strncasecmp(s, "multiedit", 9) == 0) return TOOL_EDIT;
    if (strncasecmp(s, "read",    4) == 0) return TOOL_READ;
    if (strncasecmp(s, "write",   5) == 0) return TOOL_WRITE;
    if (strncasecmp(s, "bash",    4) == 0) return TOOL_BASH;
    if (strncasecmp(s, "web",     3) == 0) return TOOL_WEB;
    if (strncasecmp(s, "fetch",   5) == 0) return TOOL_WEB;
    if (strncasecmp(s, "grep",    4) == 0) return TOOL_GREP;
    if (strncasecmp(s, "glob",    4) == 0) return TOOL_GLOB;
    if (strncasecmp(s, "task",    4) == 0) return TOOL_TASK;
    if (strncasecmp(s, "agent",   5) == 0) return TOOL_TASK;
    return TOOL_OTHER;
}

static void draw_tool_overlay(void)
{
    if (s_tool == TOOL_NONE) return;

    // Hug the top-right corner: 13px from right edge, 14px from top.
    // 22px diameter plate (was 18px) for better glyph readability.
    int bx = LCD_WIDTH - 13;
    int by = 14;
    uint16_t fg = C_WHITE;
    uint16_t accent = RGB565(255, 220, 90);
    uint16_t bg = RGB565(60, 30, 10);

    // Common circular plate behind glyph for legibility
    gc9107_fill_circle(bx, by, 11, bg);
    gc9107_fill_circle(bx, by, 10, RGB565(40, 20, 5));

    switch (s_tool) {
    case TOOL_EDIT: {
        // Pencil: thick diagonal shaft + eraser + tip
        for (int i = -7; i <= 7; i++) {
            gc9107_draw_pixel(bx + i,     by - i,     fg);
            gc9107_draw_pixel(bx + i + 1, by - i,     fg);
            gc9107_draw_pixel(bx + i,     by - i + 1, fg);
            gc9107_draw_pixel(bx + i + 1, by - i + 1, fg);
        }
        gc9107_fill_rect(bx + 4, by - 8, 4, 4, accent);  // eraser
        gc9107_fill_rect(bx - 8, by + 6, 3, 3, accent);   // graphite tip
        break;
    }
    case TOOL_READ: {
        // Open book: two pages with spine + text lines
        gc9107_fill_rect(bx - 9, by - 6, 8, 13, fg);
        gc9107_fill_rect(bx + 1, by - 6, 8, 13, fg);
        gc9107_draw_vline(bx,     by - 6, 13, accent);
        gc9107_draw_hline(bx - 8, by - 3, 7, bg);
        gc9107_draw_hline(bx + 2, by - 3, 7, bg);
        gc9107_draw_hline(bx - 8, by,     7, bg);
        gc9107_draw_hline(bx + 2, by,     7, bg);
        gc9107_draw_hline(bx - 8, by + 3, 5, bg);
        gc9107_draw_hline(bx + 2, by + 3, 5, bg);
        break;
    }
    case TOOL_WRITE: {
        // Page with text lines + folded corner
        gc9107_fill_rect(bx - 7, by - 8, 14, 17, fg);
        gc9107_fill_rect(bx + 3, by - 8, 4, 4,  bg);  // folded-corner cut
        gc9107_draw_pixel(bx + 3, by - 5, accent);
        gc9107_draw_pixel(bx + 4, by - 4, accent);
        gc9107_draw_hline(bx - 5, by - 3, 8,  bg);
        gc9107_draw_hline(bx - 5, by,     10, bg);
        gc9107_draw_hline(bx - 5, by + 3, 10, bg);
        gc9107_draw_hline(bx - 5, by + 6, 8,  bg);
        break;
    }
    case TOOL_BASH: {
        // Chevron ">" + underscore cursor
        for (int i = 0; i < 5; i++) {
            gc9107_draw_pixel(bx - 7 + i, by - 5 + i, fg);
            gc9107_draw_pixel(bx - 6 + i, by - 5 + i, fg);
            gc9107_draw_pixel(bx - 7 + i, by - 4 + i, fg);
        }
        for (int i = 0; i < 5; i++) {
            gc9107_draw_pixel(bx - 3 + i, by + 0 - i, fg);
            gc9107_draw_pixel(bx - 2 + i, by + 0 - i, fg);
            gc9107_draw_pixel(bx - 3 + i, by + 1 - i, fg);
        }
        gc9107_fill_rect(bx + 1, by + 5, 7, 2, accent);  // cursor underscore
        break;
    }
    case TOOL_WEB: {
        // Globe: filled blue disc + latitude/longitude lines + meridian curve
        gc9107_fill_circle(bx, by, 9, fg);
        gc9107_fill_circle(bx, by, 8, RGB565(80, 130, 220));
        gc9107_draw_hline(bx - 8, by,     17, fg);
        gc9107_draw_hline(bx - 7, by - 4, 15, fg);
        gc9107_draw_hline(bx - 7, by + 4, 15, fg);
        gc9107_draw_vline(bx,     by - 8, 17, fg);
        // Curved meridian (left)
        gc9107_draw_pixel(bx - 4, by - 7, fg);
        gc9107_draw_pixel(bx - 5, by - 5, fg);
        gc9107_draw_pixel(bx - 5, by + 5, fg);
        gc9107_draw_pixel(bx - 4, by + 7, fg);
        break;
    }
    case TOOL_GREP: {
        // Magnifying glass: thicker circle + longer diagonal handle
        gc9107_fill_circle(bx - 2, by - 2, 7, fg);
        gc9107_fill_circle(bx - 2, by - 2, 5, RGB565(80, 130, 220));
        for (int i = 0; i < 7; i++) {
            gc9107_draw_pixel(bx + 3 + i, by + 3 + i, fg);
            gc9107_draw_pixel(bx + 4 + i, by + 3 + i, fg);
            gc9107_draw_pixel(bx + 3 + i, by + 4 + i, fg);
        }
        break;
    }
    case TOOL_GLOB: {
        // Big asterisk
        for (int i = -8; i <= 8; i++) {
            gc9107_draw_pixel(bx + i, by,     fg);
            gc9107_draw_pixel(bx,     by + i, fg);
            gc9107_draw_pixel(bx + i, by + i, fg);
            gc9107_draw_pixel(bx + i, by - i, fg);
        }
        gc9107_fill_rect(bx - 1, by - 1, 3, 3, accent);
        break;
    }
    case TOOL_TASK: {
        // Gear: ring with 4 teeth + hub
        gc9107_fill_circle(bx, by, 8, fg);
        gc9107_fill_circle(bx, by, 4, bg);
        gc9107_fill_rect(bx - 2, by - 10, 4, 4, fg);
        gc9107_fill_rect(bx - 2, by + 6,  4, 4, fg);
        gc9107_fill_rect(bx - 10, by - 2, 4, 4, fg);
        gc9107_fill_rect(bx + 6,  by - 2, 4, 4, fg);
        gc9107_fill_circle(bx, by, 2, accent);  // hub dot
        break;
    }
    case TOOL_OTHER:
    default: {
        // Three dots (bigger)
        gc9107_fill_rect(bx - 7, by - 2, 4, 4, fg);
        gc9107_fill_rect(bx - 2, by - 2, 4, 4, fg);
        gc9107_fill_rect(bx + 3, by - 2, 4, 4, fg);
        break;
    }
    }
}

// ─── Bottom status bar ───────────────────────────────────────────────────────
// Reserved zone y=BAR_Y..LCD_HEIGHT-1. Each segment is colored by its
// per-session state in s_bar_states[] (T/W/D/.). If no bar data has been
// received yet, falls back to a single segment of the caller-supplied color.

#define BAR_Y 118
#define BAR_H 10

static uint16_t bar_seg_color(char state, uint16_t fallback)
{
    switch (state) {
    case 'T': return C_DIM;
    case 'W': return C_BLUE_DIM;
    case 'D': return C_DONE_DIM;
    case '.': return C_GREY_DIM;
    default:  return fallback;
    }
}

static void draw_bottom_bar(uint16_t fallback)
{
    int n = s_bar_count;
    if (n < 1) {
        // No bar data yet — fall back to legacy s_sessions count + single color
        n = s_sessions;
        if (n < 1) n = 1;
        if (n > 4) n = 4;
        int seg_w = LCD_WIDTH / n;
        for (int i = 0; i < n; i++) {
            int x = i * seg_w;
            int w = (i == n - 1) ? (LCD_WIDTH - x) : seg_w;
            gc9107_fill_rect(x, BAR_Y, w, BAR_H, fallback);
            if (i > 0) gc9107_fill_rect(x, BAR_Y, 1, BAR_H, RGB565(0,0,0));
        }
        return;
    }
    if (n > 4) n = 4;

    int seg_w = LCD_WIDTH / n;
    for (int i = 0; i < n; i++) {
        int x = i * seg_w;
        int w = (i == n - 1) ? (LCD_WIDTH - x) : seg_w;
        uint16_t bg = bar_seg_color(s_bar_states[i], fallback);
        gc9107_fill_rect(x, BAR_Y, w, BAR_H, bg);
        if (i > 0) gc9107_fill_rect(x, BAR_Y, 1, BAR_H, RGB565(0,0,0));
        // Label centered inside the segment (white on segment color)
        const char *lbl = s_labels[i];
        if (lbl[0]) {
            int len  = (int)strlen(lbl);
            int text_w = len * 6 - 1;  // 5px char + 1px gap, scale 1, no trailing gap
            int tx = x + (w - text_w) / 2;
            if (tx < x + 1) tx = x + 1;  // keep clear of divider
            gc9107_draw_string(tx, BAR_Y + 1, lbl, C_WHITE, bg, 1);
        }
    }
}

// ─── Screen renderers ─────────────────────────────────────────────────────────

static void draw_disconnected(void)
{
    gc9107_fill_screen(C_GREY_BG);
    draw_centred(8, "CLAUDE CODE", C_GREY_DIM, C_GREY_BG, 1);
    int ox = (int)s_eye_ox;
    draw_eyes(EYE_L_X+ox, EYE_R_X+ox, 42, blink_eye_state(), C_GREY_EYE);
    draw_centred(78, "Connect via BLE:", C_GREY_DIM, C_GREY_BG, 1);
    draw_centred(92, DEVICE_NAME,        C_GREY_LT,  C_GREY_BG, 1);
    draw_centred(106, "waiting...",      C_GREY_DIM, C_GREY_BG, 1);
    draw_bottom_bar(C_GREY_DIM);
    draw_theme_overlay();
}

static void draw_standby(void)
{
    gc9107_fill_screen(C_ORANGE);
    draw_centred(6, "CLAUDE CODE", C_FAINT, C_ORANGE, 1);
    int ox = (int)s_eye_ox;

    eye_state_t L = blink_eye_state();
    eye_state_t R = L;
    int extra_y = 0;
    if (s_blink_phase != 0) {
        if (s_blink_variant == BLINK_WINK_L) R = EYE_OPEN;
        else if (s_blink_variant == BLINK_WINK_R) L = EYE_OPEN;
        else if (s_blink_variant == BLINK_EYE_ROLL) { L = R = EYE_OPEN; extra_y = -4; }
    }
    // MOOD_TIRED: half-lid eyes when not mid-blink; slight downward droop.
    // MOOD_ALERT / MOOD_MELLOW / MOOD_DEFAULT: no shape change — only backlight differs.
    if (s_mood == MOOD_TIRED && s_blink_phase == 0) {
        L = R = EYE_HALF;
        extra_y = 2;
    }
    draw_eyes_split(EYE_L_X+ox, EYE_R_X+ox, EYE_Y+extra_y, L, R, C_EYE);
    draw_centred(98, "BLE connected", C_FAINT, C_ORANGE, 1);
    draw_bottom_bar(C_DIM);
    draw_theme_overlay();
}

static void draw_thinking(void)
{
    gc9107_fill_screen(C_ORANGE);
    static int s_dot_frame = 0;
    s_dot_frame++;
    static const char *dot_frames[] = {".  ",".. ","...","  ."," ..","   "};
    draw_centred(8, dot_frames[(s_dot_frame/8)%6], C_DIM, C_ORANGE, 2);
    int ox=(int)s_eye_ox;
    int oy=(int)s_eye_oy;

    eye_state_t L = blink_eye_state();
    eye_state_t R = L;
    int extra_y = 0;
    if (s_blink_phase != 0) {
        if (s_blink_variant == BLINK_WINK_L) R = EYE_OPEN;
        else if (s_blink_variant == BLINK_WINK_R) L = EYE_OPEN;
        else if (s_blink_variant == BLINK_EYE_ROLL) { L = R = EYE_OPEN; extra_y = -4; }
    }
    draw_eyes_split(EYE_L_X+ox, EYE_R_X+ox, EYE_Y+oy+extra_y, L, R, C_EYE);
    draw_centred(98, "thinking...", C_DIM, C_ORANGE, 1);
    draw_bottom_bar(C_DIM);
    // Tool icon replaces theme badge while a tool is active
    if (s_tool != TOOL_NONE) draw_tool_overlay();
    else                     draw_theme_overlay();
}

static void draw_done(void)
{
    gc9107_fill_screen(C_DONE_BG);
    draw_centred(8, "DONE!", C_CHECK, C_DONE_BG, 1);
    int ox = (int)s_eye_ox;
    draw_eyes(EYE_L_X+ox, EYE_R_X+ox, 42, EYE_HAPPY, C_EYE);
    draw_thick_line(45,73, 57,85, 5, C_TICK);
    draw_thick_line(57,85, 86,56, 5, C_TICK);
    int64_t secs = (esp_timer_get_time() - s_notify_us) / 1000000LL;
    char elapsed[20];
    if (secs < 5)       snprintf(elapsed, sizeof(elapsed), "just now");
    else if (secs < 60) snprintf(elapsed, sizeof(elapsed), "%llds ago", (long long)secs);
    else                snprintf(elapsed, sizeof(elapsed), "%lldm ago", (long long)(secs/60));
    draw_centred(100, elapsed, C_DONE_DIM, C_DONE_BG, 1);
    draw_bottom_bar(C_DONE_DIM);
    draw_theme_overlay();
}

static void draw_waiting(void)
{
    gc9107_fill_screen(C_BLUE_BG);
    draw_centred(5, "?", C_BLUE_LT, C_BLUE_BG, 3);
    int ox = (int)s_eye_ox;
    draw_pixel_eye(EYE_L_X+ox, EYE_Y, &BMP_CHEVRON_R[0][0], 5, C_BLUE_EYE);
    draw_pixel_eye(EYE_R_X+ox, EYE_Y, &BMP_CHEVRON_L[0][0], 5, C_BLUE_EYE);
    draw_centred(96, "your input?", C_BLUE_LT, C_BLUE_BG, 1);
    draw_bottom_bar(C_BLUE_DIM);
    draw_theme_overlay();
}

static void draw_dizzy(void)
{
    gc9107_fill_screen(C_DIZZY_BG);
    draw_centred(6, "WHOA!", C_DIZZY_LT, C_DIZZY_BG, 1);
    // Wobbling eyes — swap spiral every few frames
    static int s_spin = 0;
    s_spin++;
    const uint8_t *bmp = (s_spin / 4) & 1 ? &BMP_SPIRAL_A[0][0] : &BMP_SPIRAL_B[0][0];
    int wobble_x = (int)(4.0f * sinf(s_spin * 0.4f));
    int wobble_y = (int)(3.0f * cosf(s_spin * 0.55f));
    draw_pixel_eye(EYE_L_X+wobble_x, EYE_Y+wobble_y, bmp, 4, C_DIZZY_EYE);
    draw_pixel_eye(EYE_R_X-wobble_x, EYE_Y-wobble_y, bmp, 4, C_DIZZY_EYE);
    // Stars/asterisks above head
    const char *stars[] = {"* + *", "+ * +", "* . *"};
    draw_centred(76, stars[(s_spin/6)%3], C_DIZZY_LT, C_DIZZY_BG, 1);
    draw_centred(98, "...dizzy...", C_DIZZY_DIM, C_DIZZY_BG, 1);
}

static void draw_sleepy(void)
{
    gc9107_fill_screen(C_SLEEPY_BG);
    static int s_zzz = 0;
    s_zzz++;
    // Drifting Z's
    const char *z_frames[] = {"z", "Z", "Zz", "ZZ"};
    draw_centred(8, z_frames[(s_zzz/16)%4], C_SLEEPY_LT, C_SLEEPY_BG, 2);
    int ox = (int)s_eye_ox;
    draw_pixel_eye(EYE_L_X+ox, EYE_Y+4, &BMP_SLEEP[0][0], 2, C_SLEEPY_LT);
    draw_pixel_eye(EYE_R_X+ox, EYE_Y+4, &BMP_SLEEP[0][0], 2, C_SLEEPY_LT);
    draw_centred(98, "...zzz...", C_SLEEPY_DIM, C_SLEEPY_BG, 1);
}

static void draw_disco(void)
{
    static int s_party = 0;
    s_party++;
    // Cycle background through rainbow
    const uint16_t hues[] = {
        RGB565(255,  60,  60),
        RGB565(255, 165,  20),
        RGB565(255, 235,  40),
        RGB565( 60, 220,  90),
        RGB565( 40, 160, 240),
        RGB565(180,  70, 240),
    };
    uint16_t bg = hues[(s_party/3) % 6];
    uint16_t fg = hues[(s_party/3 + 3) % 6];
    gc9107_fill_screen(bg);
    draw_centred(6, "* PARTY *", fg, bg, 1);
    int ox = (int)(8.0f * sinf(s_party * 0.5f));
    int oy = (int)(4.0f * cosf(s_party * 0.4f));
    draw_pixel_eye(EYE_L_X+ox, EYE_Y+oy, &BMP_STAR[0][0], 4, fg);
    draw_pixel_eye(EYE_R_X-ox, EYE_Y+oy, &BMP_STAR[0][0], 4, fg);
    draw_centred(98, "DISCO MODE!", fg, bg, 1);
}

// ─── BLE command callback (called from NimBLE task) ──────────────────────────

static void on_ble_cmd(ble_cmd_t cmd, const char *arg)
{
    switch (cmd) {
    case BLE_CMD_THINKING:
        s_think_tick   = 0;
        s_eye_oy       = 0.0f;
        s_wander_timer = 1;
        enter_state(APP_THINKING);
        break;
    case BLE_CMD_WAITING:
        s_wander_timer = 1;
        enter_state(APP_WAITING);
        break;
    case BLE_CMD_DONE:
        s_notify_us = esp_timer_get_time();
        enter_state(APP_DONE);
        break;
    case BLE_CMD_STANDBY:
        enter_state(APP_STANDBY);
        break;
    case BLE_CMD_DIZZY:
        enter_dizzy();
        break;
    case BLE_CMD_THEME:
        if (!arg) { s_theme = THEME_DEFAULT; break; }
        if      (strncmp(arg, "weekend",    7) == 0) s_theme = THEME_WEEKEND;
        else if (strncmp(arg, "cny",        3) == 0) s_theme = THEME_CNY;
        else if (strncmp(arg, "christmas",  9) == 0) s_theme = THEME_CHRISTMAS;
        else if (strncmp(arg, "national",   8) == 0) s_theme = THEME_NATIONAL_DAY;
        else if (strncmp(arg, "deepavali",  9) == 0) s_theme = THEME_DEEPAVALI;
        else if (strncmp(arg, "new_year",   8) == 0) s_theme = THEME_NEW_YEAR;
        else                                          s_theme = THEME_DEFAULT;
        ESP_LOGI(TAG, "theme set: %s -> %d", arg, s_theme);
        break;
    case BLE_CMD_TOOL:
        s_tool = parse_tool_name(arg);
        s_tool_until_us = (s_tool == TOOL_NONE) ? 0 : esp_timer_get_time() + TOOL_TTL_US;
        ESP_LOGI(TAG, "tool set: %s -> %d", arg ? arg : "(clear)", s_tool);
        break;
    case BLE_CMD_TIME: {
        if (!arg) break;
        int h = atoi(arg);
        if (h < 0 || h > 23) break;
        s_hour = h;
        if      (h >= 6  && h < 10) s_mood = MOOD_ALERT;
        else if (h >= 10 && h < 17) s_mood = MOOD_DEFAULT;
        else if (h >= 17 && h < 22) s_mood = MOOD_MELLOW;
        else                        s_mood = MOOD_TIRED;
        ESP_LOGI(TAG, "time set: hour=%d mood=%d", h, s_mood);
        break;
    }
    case BLE_CMD_SESSIONS: {
        if (!arg) break;
        int n = atoi(arg);
        if (n < 0) n = 0;
        if (n > 4) n = 4;
        s_sessions = n;
        ESP_LOGI(TAG, "sessions: %d", n);
        break;
    }
    case BLE_CMD_BAR: {
        if (!arg) break;
        int n = 0;
        for (int i = 0; i < 4 && arg[i] && arg[i] != '\n' && arg[i] != '\r'; i++) {
            char c = arg[i];
            s_bar_states[i] = (c == 'T' || c == 'W' || c == 'D') ? c : '.';
            n++;
        }
        s_bar_count = n;
        ESP_LOGI(TAG, "bar: %.*s (n=%d)", n, arg, n);
        break;
    }
    case BLE_CMD_LABELS: {
        if (!arg) break;
        // Parse pipe-separated labels into s_labels[4][LABEL_MAX]
        for (int i = 0; i < 4; i++) s_labels[i][0] = 0;
        int slot = 0, col = 0;
        for (int i = 0; arg[i] && arg[i] != '\n' && arg[i] != '\r' && slot < 4; i++) {
            if (arg[i] == '|') {
                s_labels[slot][col] = 0;
                slot++;
                col = 0;
            } else if (col < LABEL_MAX - 1) {
                s_labels[slot][col++] = arg[i];
            }
        }
        if (slot < 4) s_labels[slot][col] = 0;
        ESP_LOGI(TAG, "labels: %s", arg);
        break;
    }
    }
}

// ─── Orientation polling ──────────────────────────────────────────────────────

static void poll_orientation(int16_t ax, int16_t ay, int16_t az)
{
    if (abs(az) > abs(ax) && abs(az) > abs(ay)) return;

    uint8_t new_rot = (abs(ax) > abs(ay)) ? (ax>0?0:2) : (ay>0?3:1);

    static uint8_t s_pending      = 3;
    static int     s_stable_count = 0;

    if (new_rot == s_pending) {
        if (++s_stable_count >= 3 && new_rot != s_current_rot) {
            s_current_rot = new_rot;
            gc9107_set_rotation(new_rot);
        }
    } else {
        s_pending      = new_rot;
        s_stable_count = 0;
    }
}

// ─── Button: debounce + short/long classification + Konami ──────────────────

static void konami_record(bool long_press)
{
    int64_t now = esp_timer_get_time();
    if (now - s_konami_last_us > 4000000LL) {
        s_konami_count = 0;
    }
    s_konami_last_us = now;

    // Shift left, append new press
    for (int i = 0; i < KONAMI_LEN - 1; i++) s_konami_buf[i] = s_konami_buf[i+1];
    s_konami_buf[KONAMI_LEN-1] = long_press ? 1 : 0;
    if (s_konami_count < KONAMI_LEN) s_konami_count++;

    if (s_konami_count == KONAMI_LEN) {
        bool match = true;
        for (int i = 0; i < KONAMI_LEN; i++) {
            if (s_konami_buf[i] != KONAMI_PATTERN[i]) { match = false; break; }
        }
        if (match) {
            ESP_LOGI(TAG, "konami matched → disco");
            enter_disco();
            // reset to prevent immediate retrigger
            for (int i = 0; i < KONAMI_LEN; i++) s_konami_buf[i] = 9;
            s_konami_count = 0;
        }
    }
}

// ─── Entry point ──────────────────────────────────────────────────────────────

void app_main(void)
{
    gc9107_init();

    if (lp5562_init(&s_sys_bus)) lp5562_set_backlight(s_sys_bus, 200);
    s_imu_ok = bmi270_init(s_sys_bus);

    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BTN_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn);

    ble_nus_init(DEVICE_NAME, on_ble_cmd);

    srand((unsigned)esp_timer_get_time());
    s_blink_timer = 60 + rand() % 60;
    s_last_change_us = esp_timer_get_time();

    int     btn_debounce  = 0;
    bool    btn_held      = false;
    int64_t btn_press_us  = 0;
    bool    s_was_connected = false;

    int imu_orient_tick = 0;
    uint8_t cur_backlight = 200;

    for (;;) {
        int64_t now = esp_timer_get_time();
        bool now_connected = ble_nus_connected();
        if (now_connected && !s_was_connected) {
            if (s_app_state == APP_DISCONNECTED) enter_state(APP_STANDBY);
        }
        if (!now_connected && s_was_connected) {
            enter_state(APP_DISCONNECTED);
        }
        s_was_connected = now_connected;

        // ── Button: debounce + short/long classification ─────────────────────
        bool raw_pressed = (gpio_get_level(BTN_GPIO) == 0);
        if (raw_pressed) {
            if (btn_debounce < 3) btn_debounce++;
            if (btn_debounce == 3 && !btn_held) {
                btn_held = true;
                btn_press_us = now;
            }
        } else {
            if (btn_held) {
                int64_t dur_ms = (now - btn_press_us) / 1000;
                bool long_press = dur_ms >= 300;
                konami_record(long_press);
                // Existing single-press behavior: wake / standby on any press
                if (s_app_state == APP_SLEEPY) enter_state(APP_STANDBY);
                else if (s_app_state != APP_DISCO) {
                    enter_state(now_connected ? APP_STANDBY : APP_DISCONNECTED);
                }
            }
            btn_debounce = 0;
            btn_held = false;
        }

        // ── IMU: tilt + shake every frame; orientation every 10 frames ───────
        int16_t ax = 0, ay = 0, az = 0;
        bool imu_ok = s_imu_ok && bmi270_read_accel(&ax, &ay, &az);
        if (imu_ok) {
            if (detect_shake(ax, ay, az)) {
                if (s_app_state != APP_DISCO) enter_dizzy();
            }
            if (++imu_orient_tick >= 10) {
                imu_orient_tick = 0;
                poll_orientation(ax, ay, az);
            }
        }

        // ── Auto-dismiss DONE after 5 min ────────────────────────────────────
        if (s_app_state == APP_DONE) {
            int64_t age = (now - s_notify_us) / 1000000LL;
            if (age > 300) enter_state(now_connected ? APP_STANDBY : APP_DISCONNECTED);
        }

        // ── Tool icon TTL: clear if daemon went silent for >60s ─────────────
        if (s_tool != TOOL_NONE && now > s_tool_until_us) {
            s_tool = TOOL_NONE;
        }
        // Tool icon only makes sense during THINKING; clear on state change away
        if (s_app_state != APP_THINKING && s_tool != TOOL_NONE) {
            s_tool = TOOL_NONE;
        }

        // ── Dizzy / disco timeouts ───────────────────────────────────────────
        if (s_app_state == APP_DIZZY && now > s_dizzy_until_us) {
            s_app_state = s_prev_state;
            mark_event();
        }
        if (s_app_state == APP_DISCO && now > s_disco_until_us) {
            s_app_state = s_prev_state;
            mark_event();
        }

        // ── Sleepy: enter from standby after 5 min idle ──────────────────────
        if (s_app_state == APP_STANDBY && (now - s_last_change_us) > SLEEPY_IDLE_US) {
            s_app_state = APP_SLEEPY;
            // don't mark_event() here — we want SLEEPY to be the stable state
        }

        // ── Backlight: dim in sleepy, party-flash in disco, mood-adjusted otherwise
        uint8_t want_bl = 200;
        switch (s_mood) {
        case MOOD_ALERT:   want_bl = 230; break;
        case MOOD_DEFAULT: want_bl = 200; break;
        case MOOD_MELLOW:  want_bl = 150; break;
        case MOOD_TIRED:   want_bl = 100; break;
        }
        if (s_app_state == APP_SLEEPY) want_bl = 30;
        else if (s_app_state == APP_DISCO) {
            // strobe between 80 and 255
            want_bl = ((now / 100000) & 1) ? 255 : 80;
        }
        if (want_bl != cur_backlight) {
            lp5562_set_backlight(s_sys_bus, want_bl);
            cur_backlight = want_bl;
        }

        // ── Animation tick ───────────────────────────────────────────────────
        if (s_app_state == APP_THINKING) {
            update_blink();
            update_thinking_motion();
        } else if (s_app_state == APP_SLEEPY || s_app_state == APP_DIZZY || s_app_state == APP_DISCO) {
            // these screens animate their own way
        } else {
            update_blink();
            update_wander();
        }

        // ── Draw ─────────────────────────────────────────────────────────────
        switch (s_app_state) {
        case APP_DISCONNECTED: draw_disconnected(); break;
        case APP_STANDBY:      draw_standby();      break;
        case APP_THINKING:     draw_thinking();     break;
        case APP_DONE:         draw_done();         break;
        case APP_WAITING:      draw_waiting();      break;
        case APP_DIZZY:        draw_dizzy();        break;
        case APP_SLEEPY:       draw_sleepy();       break;
        case APP_DISCO:        draw_disco();        break;
        }
        gc9107_flush();

        vTaskDelay(pdMS_TO_TICKS(33));
    }
}
