// TV playback: an MPEG-TS stream from the bridge (H.264 Constrained Baseline with one slice per
// frame, up to 240x240, AAC-LC) decoded and shown on the panel, its sound on the speaker. The
// sound's position paces the frames; late frames are dropped rather than slowing the sound.
// The screen belongs to the picture while it plays; a tap or the PWR key stops it.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Builds the colour tables and borrows LVGL's draw buffers; call once LVGL is up.
void tv_init(void);
// live: an endless channel (kept near the live edge, reconnected when it breaks); false for a
// recording, which ends. Returns at once; the screen stays the menu's until the first picture,
// and tv_error() says why if it ends early.
bool tv_play(const char *url, const char *title, bool live);
void tv_stop(void);
bool tv_playing(void);
bool tv_stopping(void); // asked to stop, not finished yet
bool tv_busy(void);     // the last start failed because the bridge was busy
// Why the last playback ended (or is ending): "end", "tap", "stopped", "speaker taken", "error".
const char *tv_end_reason(void);
const char *tv_error(void);
int tv_first_picture_ms(void); // from the start to the first picture, 0 if none yet
// The next frame drawn, as I420 (w x h, Y stride w; it sits centred on the screen), for
// /api/screen. NULL if no picture is up or none came within timeout_ms. The buffer stays
// valid until the next call; call from one task only.
const uint8_t *tv_capture(int *w, int *h, int timeout_ms);
// Touches while the picture has the screen, from the draw task (keep the handler short). Without
// a handler a tap stops playback.
typedef enum { TV_NONE, TV_TAP, TV_SWIPE_UP, TV_SWIPE_DOWN, TV_SWIPE_LEFT, TV_SWIPE_RIGHT } tv_touch_t;
typedef void (*tv_touch_fn)(tv_touch_t what, int x, int y);
void tv_set_touch_handler(tv_touch_fn fn);
// A full-screen image (RGB565 in the panel's byte order, kept by the caller) drawn instead of
// the picture until tv_overlay(NULL); the sound and the decoding go on underneath.
void tv_overlay(const uint16_t *img);
// {"title":..,"playing":..,"live":..,"fps":..,"shown":..,"dropped":..,...,"source":{...}}
void tv_status_json(char *out, size_t len);
