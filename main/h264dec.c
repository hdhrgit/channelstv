// H.264 for the video player, see h264dec.h. Decoding is Espressif's tinyH264 port; the
// colour conversion is table driven, since the straightforward version cost ~12 ms a frame.

#include "h264dec.h"
#include "board.h"

#include <stdlib.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_h264_dec_sw.h"
#include "esp_timer.h"

struct h264dec {
    esp_h264_dec_handle_t dec;
    int coded_w, coded_h;
};

// Per-sample contributions and the 5/6/5 bits of a clamped channel value, already in place
// and byte-swapped for the panel. With contrast and saturation up to 2x, channel sums run
// from about -730 to 990, hence the 768 offset into 2048-entry tables.
#define CLAMP_OFF 768
#define CLAMP_N   2048
static int16_t s_y[256], s_rv[256], s_gu[256], s_gv[256], s_bu[256];
static uint16_t *s_r, *s_g, *s_b;

// Picture adjustments, in hundredths: saturation and contrast 100 = as encoded, brightness
// 0 = as encoded (added to luma).
static int s_sat = 100, s_contrast = 100, s_bright = 0;

static void tables_build(void)
{
    for (int i = 0; i < 256; i++) {
        // luma: studio range to full, then contrast around mid-grey and brightness
        int y = (298 * (i - 16)) >> 8;
        s_y[i] = (int16_t)(((y - 128) * s_contrast) / 100 + 128 + s_bright);
        int c = ((i - 128) * s_sat) / 100; // chroma, scaled by saturation
        s_rv[i] = (int16_t)((409 * c + 128) >> 8);
        s_gu[i] = (int16_t)((-100 * c) >> 8);
        s_gv[i] = (int16_t)((-208 * c + 128) >> 8);
        s_bu[i] = (int16_t)((516 * c + 128) >> 8);
    }
}

static void tables_init(void)
{
    if (s_r) {
        return;
    }
    tables_build();
    // internal RAM: these are hit three times per pixel
    s_r = heap_caps_malloc(3 * CLAMP_N * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
    if (!s_r) {
        s_r = malloc(3 * CLAMP_N * sizeof(uint16_t));
    }
    s_g = s_r + CLAMP_N;
    s_b = s_g + CLAMP_N;
    for (int i = 0; i < CLAMP_N; i++) {
        int v = i - CLAMP_OFF;
        v = v < 0 ? 0 : v > 255 ? 255 : v;
        uint16_t r = (v & 0xf8) << 8, g = (v & 0xfc) << 3, b = v >> 3;
        s_r[i] = r >> 8 | r << 8;
        s_g[i] = g >> 8 | g << 8;
        s_b[i] = b >> 8 | b << 8;
    }
}

void h264_set_picture(int saturation, int contrast, int brightness)
{
    // limits keep every channel sum inside the clamp tables
    s_sat = saturation < 0 ? 0 : saturation > 200 ? 200 : saturation;
    s_contrast = contrast < 20 ? 20 : contrast > 200 ? 200 : contrast;
    s_bright = brightness < -50 ? -50 : brightness > 50 ? 50 : brightness;
    tables_build(); // a frame converted meanwhile may mix old and new values; harmless
}

void h264_picture(int *saturation, int *contrast, int *brightness)
{
    *saturation = s_sat;
    *contrast = s_contrast;
    *brightness = s_bright;
}

void h264_i420_rows_to_rgb565be(const uint8_t *py, const uint8_t *pu, const uint8_t *pv, int stride, int w, int h,
                                uint16_t *out)
{
    for (int y = 0; y < h; y += 2) {
        const uint8_t *y0 = py + y * stride, *y1 = y0 + stride;
        const uint8_t *u = pu + (y / 2) * (stride / 2), *v = pv + (y / 2) * (stride / 2);
        uint16_t *o0 = out + y * w, *o1 = o0 + w;
        bool two = y + 1 < h;
        for (int x = 0; x < w; x += 2) {
            int r = s_rv[*v] + CLAMP_OFF, g = s_gu[*u] + s_gv[*v] + CLAMP_OFF, b = s_bu[*u] + CLAMP_OFF;
            u++;
            v++;
            int c = s_y[y0[x]];
            o0[x] = s_r[c + r] | s_g[c + g] | s_b[c + b];
            c = s_y[y0[x + 1]];
            o0[x + 1] = s_r[c + r] | s_g[c + g] | s_b[c + b];
            if (two) {
                c = s_y[y1[x]];
                o1[x] = s_r[c + r] | s_g[c + g] | s_b[c + b];
                c = s_y[y1[x + 1]];
                o1[x + 1] = s_r[c + r] | s_g[c + g] | s_b[c + b];
            }
        }
    }
}

void h264_i420_turned_to_rgb565be(const uint8_t *py, const uint8_t *pu, const uint8_t *pv, int w, int h, int rot,
                                  int first, int lines, uint16_t *out)
{
    board_turn_t t = board_turn(w, h, rot, first, lines);
    for (int y = t.y0; y < t.y1; y += 2) {
        const uint8_t *y0 = py + y * w, *y1 = y0 + w;
        const uint8_t *u = pu + (y / 2) * (w / 2), *v = pv + (y / 2) * (w / 2);
        uint16_t *o = out + t.k0 + t.ky * y;
        for (int x = t.x0; x < t.x1; x += 2) {
            int r = s_rv[v[x / 2]] + CLAMP_OFF, g = s_gu[u[x / 2]] + s_gv[v[x / 2]] + CLAMP_OFF, b = s_bu[u[x / 2]] + CLAMP_OFF;
            uint16_t *p = o + t.kx * x;
            int c = s_y[y0[x]];
            p[0] = s_r[c + r] | s_g[c + g] | s_b[c + b];
            c = s_y[y0[x + 1]];
            p[t.kx] = s_r[c + r] | s_g[c + g] | s_b[c + b];
            c = s_y[y1[x]];
            p[t.ky] = s_r[c + r] | s_g[c + g] | s_b[c + b];
            c = s_y[y1[x + 1]];
            p[t.kx + t.ky] = s_r[c + r] | s_g[c + g] | s_b[c + b];
        }
    }
}

void h264_i420_to_rgb565be(const uint8_t *yuv, int stride, int coded_h, int w, int h, uint16_t *out)
{
    const uint8_t *pu = yuv + stride * coded_h, *pv = pu + (stride / 2) * (coded_h / 2);
    h264_i420_rows_to_rgb565be(yuv, pu, pv, stride, w, h, out);
}

void h264dec_init(void)
{
    tables_init();
}

h264dec_t *h264dec_open(void)
{
    tables_init();
    h264dec_t *d = calloc(1, sizeof(*d));
    esp_h264_dec_cfg_sw_t cfg = {.pic_type = ESP_H264_RAW_FMT_I420};
    if (!d || esp_h264_dec_sw_new(&cfg, &d->dec) != ESP_H264_ERR_OK || esp_h264_dec_open(d->dec) != ESP_H264_ERR_OK) {
        if (d && d->dec) {
            esp_h264_dec_del(d->dec);
        }
        free(d);
        return NULL;
    }
    return d;
}

void h264dec_close(h264dec_t *d)
{
    if (d) {
        esp_h264_dec_close(d->dec);
        esp_h264_dec_del(d->dec);
        free(d);
    }
}

int h264dec_frame(h264dec_t *d, const uint8_t *data, int len, uint8_t *out, int w, int h, int64_t *convert_us)
{
    esp_h264_dec_in_frame_t in = {.raw_data = {.buffer = (uint8_t *)data, .len = len}};
    int got = 0;
    while (in.raw_data.len > 0) {
        esp_h264_dec_out_frame_t pic = {0};
        if (esp_h264_dec_process(d->dec, &in, &pic) != ESP_H264_ERR_OK) {
            return -1;
        }
        if (!in.consume) {
            break;
        }
        in.raw_data.buffer += in.consume;
        in.raw_data.len -= in.consume;
        if (!pic.out_size) {
            continue;
        }
        if (!d->coded_w) {
            esp_h264_resolution_t res = {0};
            esp_h264_dec_param_sw_handle_t param = NULL;
            if (esp_h264_dec_sw_get_param_hd(d->dec, &param) == ESP_H264_ERR_OK) {
                esp_h264_dec_get_resolution(param, &res);
            }
            d->coded_w = res.width;
            d->coded_h = res.height;
        }
        if (w > d->coded_w || h > d->coded_h) {
            return -1;
        }
        int64_t a = esp_timer_get_time();
        // the visible w x h as compact I420 (stride w): the decoder reuses its buffer
        const uint8_t *src = pic.outbuf;
        int cw = d->coded_w;
        for (int y = 0; y < h; y++) {
            memcpy(out + y * w, src + y * cw, w);
        }
        const uint8_t *su = src + cw * d->coded_h, *sv = su + (cw / 2) * (d->coded_h / 2);
        uint8_t *ou = out + w * h, *ov = ou + (w / 2) * (h / 2);
        for (int y = 0; y < h / 2; y++) {
            memcpy(ou + y * (w / 2), su + y * (cw / 2), w / 2);
            memcpy(ov + y * (w / 2), sv + y * (cw / 2), w / 2);
        }
        if (convert_us) {
            *convert_us = esp_timer_get_time() - a;
        }
        got = 1;
    }
    return got;
}

bool h264_is_keyframe(const uint8_t *data, int len)
{
    for (int i = 0; i + 3 < len; i++) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            int type = data[i + 3] & 0x1f;
            if (type == 5) {
                return true; // IDR slice
            }
            if (type == 1) {
                return false; // an ordinary slice comes first: not a keyframe
            }
            i += 3;
        }
    }
    return false;
}
