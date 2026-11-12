// Software H.264 (baseline) for the video player: one access unit in, one I420 frame out;
// the draw task converts it to RGB565 strip by strip on the other core.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct h264dec h264dec_t;

// Builds the colour tables (12 KB of internal RAM, for good): at boot, while that RAM is still in
// one piece, rather than at the first video, where they split what the draw task needs.
void h264dec_init(void);
h264dec_t *h264dec_open(void);
void h264dec_close(h264dec_t *d);

// Decodes one access unit (Annex-B). When it yields a picture, copies the top-left w x h of
// it (the coded size is rounded up to 16) into out as compact I420 (w*h*3/2 bytes, Y stride
// w) and returns 1; 0 if no picture came out, -1 on a decoder error. convert_us, if given,
// gets the time the copy took.
int h264dec_frame(h264dec_t *d, const uint8_t *data, int len, uint8_t *out, int w, int h, int64_t *convert_us);
// With VideoJet the copy into out finishes on core 1 after h264dec_frame returns, while the next
// frame decodes: wait for it before reading out
void h264dec_ready(const uint8_t *out);

// True if the chunk holds a keyframe (IDR slice), where decoding can start afresh.
bool h264_is_keyframe(const uint8_t *data, int len);

// Picture adjustments applied while converting, in hundredths: saturation and contrast
// (100 = as encoded, up to 200), brightness added to luma (-50..50).
void h264_set_picture(int saturation, int contrast, int brightness);
void h264_picture(int *saturation, int *contrast, int *brightness);

// I420 (stride = coded width) to the panel's big-endian RGB565, BT.601 studio range.
void h264_i420_to_rgb565be(const uint8_t *yuv, int stride, int coded_h, int w, int h, uint16_t *out);
// The same for h rows (even) given each plane's start: py at stride, pu and pv at stride / 2.
void h264_i420_rows_to_rgb565be(const uint8_t *py, const uint8_t *pu, const uint8_t *pv, int stride, int w, int h,
                                uint16_t *out);
// A compact w x h I420 picture (w and h even) turned rot quarter turns, as the panel gets it:
// its rows [first, first + lines), (rot & 1 ? h : w) wide (board_turn).
void h264_i420_turned_to_rgb565be(const uint8_t *py, const uint8_t *pu, const uint8_t *pv, int w, int h, int rot,
                                  int first, int lines, uint16_t *out);
// The same, a quarter turn (rot 1 or 3) and stretched to W x H on the way (nearest pixel): the
// panel block is H wide and W tall, and this makes its rows [first, first + lines).
void h264_i420_scaled_turned_to_rgb565be(const uint8_t *py, const uint8_t *pu, const uint8_t *pv, int w, int h, int W,
                                         int H, int rot, int first, int lines, uint16_t *out);
// Rows [first, first + lines) of the w x h picture scaled up to W x H upright (nearest pixel)
void h264_i420_scaled_to_rgb565be(const uint8_t *py, const uint8_t *pu, const uint8_t *pv, int w, int h, int W, int H,
                                  int first, int lines, uint16_t *out);
