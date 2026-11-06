// Auto-rotate: the motion sensor says which way is down, and the picture's top follows the
// panel edge that is up. It turns once the board has stayed that way for a second, and not
// while it lies flat. Or a fixed rotation, from Settings, the web page or /api/rotation.
#pragma once

#include <stddef.h>
#include <stdbool.h>

#define ROTATE_AUTO -1

// Reads the setting from NVS and starts watching the sensor.
void rotate_start(void);
// ROTATE_AUTO, or 0..3: the board turned that many quarter turns clockwise from upright
// (board.h). Saved in NVS.
int rotate_setting(void);
void rotate_set(int setting);
// Keeps the panel's own way up (the touch test, whose taps must be the panel's raw ones).
void rotate_hold(bool on);
// For tests: acceleration in g to use instead of the sensor's; fake false goes back to it.
void rotate_fake(bool fake, float x, float y, float z);
// {"setting":"auto"|0..3,"rotation":0..3,"auto":0..3,"accel":[x,y,z],"fake":..,"flat":..,"hold":..}
void rotate_status_json(char *out, size_t len);
