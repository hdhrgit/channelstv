// The boards, told apart at boot (board_init):
// - Waveshare ESP32-S3-Touch-LCD-1.54: 240x240 ST7789 on SPI, CST816 touch and ES8311 speaker
//   codec on one I2C bus, three keys, a battery behind a latching power switch.
// - Waveshare ESP32-S3-Touch-AMOLED-1.8: 368x448 AMOLED on QSPI (V1: SH8601 with FT3168 touch,
//   V2: CO5300 with CST816 touch), ES8311, an AXP2101 power chip with the battery and the PWR
//   key, BOOT as the only other key.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_codec_dev.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

// The panel's size, set by board_init()
extern int g_lcd_w, g_lcd_h;
#define LCD_W g_lcd_w
#define LCD_H g_lcd_h
#define LCD_MAX_W 368 // the widest of the boards' panels
#define SAMPLE_RATE 44100

// KEY_VOL_CYCLE: down in steps, from 0 round to 100 again (the AMOLED-1.8's BOOT held)
typedef enum { KEY_POWER, KEY_VOL_UP, KEY_VOL_DOWN, KEY_VOL_CYCLE } board_key_t;
// PWR: SHORT under 0.5 s; past that HOLD ten times a second, then LONG at 3 s, or CANCEL
// if let go first. Volume keys: SHORT, then REPEAT while held.
typedef enum { PRESS_SHORT, PRESS_LONG, PRESS_REPEAT, PRESS_HOLD, PRESS_CANCEL } press_t;
typedef void (*key_fn)(board_key_t key, press_t press);

typedef struct {
    int mv;          // battery voltage, 0 if unknown
    int percent;     // estimated; while charging only rising, and at most 99
    bool charging;   // the charger's status line says so
    bool usb;        // a USB host is talking to us, or we are charging
} battery_t;

// Touch calibration: screen = a * raw + b per axis, after swapping the raw axes if swap.
typedef struct {
    bool swap;
    float ax, bx, ay, by;
} touch_cal_t;
void board_touch_cal(touch_cal_t *out);
void board_touch_set_cal(const touch_cal_t *cal);
// The latest raw point the touch chip reported; false if none yet.
bool board_touch_raw(int *x, int *y);
// The last touch points, raw and mapped, oldest first: a press or a move (down), or a lift.
#define TOUCH_LOG_N 16
typedef struct {
    uint32_t ms;
    int16_t raw_x, raw_y, x, y;
    bool down;
} touch_log_t;
int board_touch_log(touch_log_t *out, int max);

typedef enum { BOARD_LCD154, BOARD_AMOLED18 } board_id_t;
typedef struct {
    board_id_t id;
    const char *name;    // "Waveshare ESP32-S3-Touch-AMOLED-1.8"
    const char *rev;     // "V2", or "" where there is only one
    const char *display; // "CO5300 368x448 QSPI"
    const char *touch;   // "CST816 (0x15)"
} board_info_t;
const board_info_t *board_info(void);
// The chips found at boot with their IDs, as a JSON array, for /api/chips.
void board_chips_json(char *buf, size_t len);

// Finds out which board this is, keeps it powered on battery, then brings up I2C, the codec
// and the LCD.
void board_init(void);
esp_codec_dev_handle_t board_speaker(void);
void board_amp(bool on);
esp_lcd_panel_io_handle_t board_lcd_io(void);
esp_lcd_panel_handle_t board_lcd_panel(void);
esp_lcd_touch_handle_t board_touch(void);
void board_backlight(int percent);
int board_backlight_percent(void);
void board_keys_start(key_fn fn);
// How long PWR has been held, in ms, as of the last PRESS_HOLD or PRESS_LONG.
int board_power_hold_ms(void);

void board_battery(battery_t *out);
float board_chip_temp(void);
// From the QMI8658 motion sensor, nearer the room than the chip is. -273 if absent.
float board_imu_temp(void);
// The QMI8658's acceleration in g on its own axes; false without it.
bool board_accel(float *x, float *y, float *z);
// The panel's rotation: how many quarter turns clockwise the board is turned from upright
// (0..3), so the picture's top is at the panel's top, left, bottom or right edge. Touches are
// turned with it. board_rotation_want() asks for one;
// board_rotation_apply() makes it, from whoever draws on the panel at the time (the LVGL task,
// or playback's draw task); true if it changed, and the whole screen needs drawing again.
// The panel itself keeps its own way up, so what is sent to it always runs along its scan:
// pictures are turned in software on their way to it (board_turn and the functions below).
void board_rotation_want(int r);
int board_rotation(void);
// The way the video is turned: board_rotation() on a square panel; on a tall one the menus stay
// upright while the video also turns a quarter, to fill the panel's height.
int board_video_rotation(void);
bool board_rotation_apply(void);

// How a w x h picture (w and h even) turned rot quarter turns lands on the panel: the panel
// block is (rot & 1 ? h : w) wide, and of its rows [first, first + lines) the source pixel
// (x, y) goes to index k0 + kx * x + ky * y, for x in [x0, x1) and y in [y0, y1).
typedef struct {
    int kx, ky, k0, x0, x1, y0, y1;
} board_turn_t;

static inline board_turn_t board_turn(int w, int h, int rot, int first, int lines)
{
    switch (rot & 3) {
    case 1: return (board_turn_t){-h, 1, (w - 1 - first) * h, w - first - lines, w - first, 0, h};
    case 2: return (board_turn_t){-1, -w, (h - 1 - first) * w + w - 1, 0, w, h - first - lines, h - first};
    case 3: return (board_turn_t){h, -1, -first * h + h - 1, first, first + lines, 0, h};
    default: return (board_turn_t){1, w, -first * w, 0, w, first, first + lines};
    }
}

// The panel position of a screen rectangle (x, y, w x h) when the screen is turned rot.
void board_turn_rect(int x, int y, int w, int h, int rot, int *px, int *py);
// Rows [first, first + lines) of a w x h RGB565 image turned rot, into dst.
void board_turn_rows(const uint16_t *src, int w, int h, int rot, int first, int lines, uint16_t *dst);
// A w x h RGB565 block turned rot in place (w * h at most LCD_MAX_W * 20).
void board_turn_in_place(uint16_t *p, int w, int h, int rot);
// On a panel with its tearing effect line (the AMOLED-1.8), waits until the panel's refresh has
// just passed panel row y, so a picture written from y downwards stays behind the refresh and
// never shows half old, half new. Returns at once without the line, with the sync off, or if
// the pulses stopped; it never waits longer than one refresh.
void board_wait_scan(int y);
// The sync on or off (for comparing by eye; not saved), and whether it is on
void board_set_te_sync(bool on);
bool board_te_sync(void);
// The panel's refresh period in us from its TE pulses, 0 without them
int board_te_period_us(void);
// The panel's gamma curve: 1.0, 1.8, 2.2 (the power-on default) or 2.5; the nearest is used.
// Call with the LVGL lock held, so it doesn't land in the middle of a frame.
bool board_panel_gamma(float gamma);
// "0x15,0x18" - what answered on the I2C bus at boot
const char *board_i2c_devices(void);
// Cuts the battery latch. Does nothing while USB power is present.
// Powers off (or on USB power, deep sleep until PWR is pressed). With wake_s the latch stays on
// and the board also wakes up by itself after that many seconds (reason: timer).
void board_power_off(unsigned wake_s);
