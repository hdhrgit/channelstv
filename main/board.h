// Waveshare ESP32-S3-Touch-LCD-1.54: 240x240 ST7789 on SPI, CST816 touch and ES8311
// speaker codec on one I2C bus, three keys, a battery behind a latching power switch.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_codec_dev.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#define LCD_W 240
#define LCD_H 240
#define SAMPLE_RATE 44100

typedef enum { KEY_POWER, KEY_VOL_UP, KEY_VOL_DOWN } board_key_t;
// PWR: SHORT under 0.5 s; past that HOLD ten times a second, then LONG at 3 s, or CANCEL
// if let go first. Volume keys: SHORT, then REPEAT while held.
typedef enum { PRESS_SHORT, PRESS_LONG, PRESS_REPEAT, PRESS_HOLD, PRESS_CANCEL } press_t;
typedef void (*key_fn)(board_key_t key, press_t press);

typedef struct {
    int mv;          // battery voltage, 0 if unknown
    int percent;
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

// Keeps the board powered on battery, then brings up I2C, the codec and the LCD.
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
// The panel's rotation, 0..3 quarter turns: the picture's top at the panel's top, right,
// bottom or left edge. Touches are turned with it. board_rotation_want() asks for one;
// board_rotation_apply() makes it, from whoever draws on the panel at the time (the LVGL task,
// or playback's draw task); true if it changed, and the whole screen needs drawing again.
void board_rotation_want(int r);
int board_rotation(void);
bool board_rotation_apply(void);
// The panel's gamma curve: 1.0, 1.8, 2.2 (the power-on default) or 2.5; the nearest is used.
// Call with the LVGL lock held, so it doesn't land in the middle of a frame.
bool board_panel_gamma(float gamma);
// "0x15,0x18" - what answered on the I2C bus at boot
const char *board_i2c_devices(void);
// Cuts the battery latch. Does nothing while USB power is present.
// Powers off (or on USB power, deep sleep until PWR is pressed). With wake_s the latch stays on
// and the board also wakes up by itself after that many seconds (reason: timer).
void board_power_off(unsigned wake_s);
