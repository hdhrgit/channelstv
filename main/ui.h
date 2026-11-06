// The touchscreen: what the bridge serves, a folder at a time (Live TV, recordings, ...), a
// page at a time; tap a folder to open it, an item to play it, the arrow (or PWR) to go back.
#pragma once

#include <stdbool.h>
#include <stdint.h>

void ui_start(void);
// Any key press: wakes a dimmed screen. Safe from any task.
void ui_wake(void);
// Up one folder. Safe from any task.
void ui_back(void);
// Flashes the volume over the screen. Safe from any task.
void ui_show_volume(int volume);
// Full brightness now, even if idle on battery had dimmed it. Call with the LVGL lock held
// (playback does, since the dimming timer can't run while it owns the screen).
void ui_backlight_full(void);
// The power-off countdown while PWR is held: held_ms of 3000 shows "Powering off",
// 0 hides it. Safe from any task.
void ui_power_hold(int held_ms);
// A touch as the screen would take it (/api/touch): gesture "tap", "long", "up", "down", "left"
// or "right", at x, y. While something plays it goes to the playback controls. False if the
// gesture is unknown or the screen was busy.
bool ui_touch(const char *gesture, int x, int y);
// Plays a stream the way a tap on an item does (/api/play). For a recording, id, dur_s,
// time_seek and last_pos_s (as the folder lists it; -1: not given) give it the seek keys, the
// position and resume. False if another start is still under way.
bool ui_play(const char *url, const char *title, bool live, const char *id, int dur_s, bool time_seek, int last_pos_s);
// Seeks the recording that plays by by_s seconds (/api/seek), as the seek keys do; false if
// it can't (live, nothing plays, or the bridge doesn't seek). *at_s: where it is (-1: not
// seekable), *dur_s: its length (0: unknown).
bool ui_seek(int by_s, int *at_s, int *dur_s);
// Takes the "Tuning" screen of a start away. Call with the LVGL lock held.
void ui_tuning_end(void);
// text in the accent colour on black, w x h, as the panel takes it (RGB565, bytes swapped), in
// PSRAM for the player to draw itself; NULL without memory. Call with LVGL's lock held.
uint16_t *ui_banner(const char *text, int w, int h);
