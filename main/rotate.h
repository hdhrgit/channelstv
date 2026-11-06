// Auto-rotate: the motion sensor says which way is down, and the picture's top follows the
// panel edge that is up. It turns once the board has stayed that way for a second, and not
// while it lies flat. Or a fixed rotation, from Settings, the web page or /api/rotation.
#pragma once

#include <stddef.h>
#include <stdbool.h>

#define ROTATE_AUTO -1

// Reads the setting from NVS and starts watching the sensor.
void rotate_start(void);
// ROTATE_AUTO, or 0..3 quarter turns (board.h: the picture's top at the panel's top, right,
// bottom or left edge). Saved in NVS.
int rotate_setting(void);
void rotate_set(int setting);
// How the sensor sits on the board, 0..7 (bit 0: its x and y axes swapped, bit 1: x reversed,
// bit 2: y reversed). Saved in NVS.
int rotate_axes(void);
void rotate_set_axes(int map);
// Keeps the panel's own way up (the touch test, whose taps must be the panel's raw ones).
void rotate_hold(bool on);
// For tests: acceleration in g to use instead of the sensor's; fake false goes back to it.
void rotate_fake(bool fake, float x, float y, float z);
// {"setting":"auto"|0..3,"rotation":0..3,"auto":0..3,"axes":0..7,"accel":[x,y,z],"fake":..,"flat":..}
void rotate_status_json(char *out, size_t len);
