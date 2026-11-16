// H.264 for the video player, see h264dec.h. Decoding is Espressif's tinyH264 port or VideoJet
// (Settings -> channelstv -> H.264 decoder); the colour conversion is table driven, since the
// straightforward version cost ~12 ms a frame.

#include "h264dec.h"
#include "board.h"

#include <stdlib.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#if CONFIG_CHANNELSTV_DEC_VIDEOJET
#include "videojet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#include "esp_h264_dec_sw.h"
#endif

struct h264dec {
#if CONFIG_CHANNELSTV_DEC_VIDEOJET
    vj_decoder_t *dec;
    // the copy of the latest frame into its output buffer, done by a task on core 1 while core 0
    // decodes the next frame (hold mode keeps the frame unchanged until then)
    TaskHandle_t copier;
    vj_frame_t job;
    uint8_t *job_out;
    int job_w, job_h;
    volatile bool quit;
#else
    esp_h264_dec_handle_t dec;
#endif
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

// The PIE version of the quarter turn (yuv_pie.S), for saturation and contrast 100: its constants,
// 8 lanes each (see there), the brightness among them
typedef struct {
    const uint8_t *y, *u, *v;
    uint16_t *o;
    int w, unused, kx, step, cols;
    const int16_t *k;
    void *tmp;
    int swap;
} yuv_pie_job_t;
void yuv_pie_turned(const yuv_pie_job_t *j);
#if CONFIG_CHANNELSTV_STAGE_TIMING
volatile int64_t h264_scale_us;
#endif
static int16_t s_pie_k[15][8] __attribute__((aligned(16)));
static int32_t s_pie_tmp[4] __attribute__((aligned(16)));

static void tables_build(void)
{
    static const int16_t k[15] = {128, 153, -208, 4, 1, -100, 16, 42, 0, 255, 0xf8, 1, 0x1c, 8, 1};
    for (int i = 0; i < 15; i++) {
        for (int l = 0; l < 8; l++) {
            s_pie_k[i][l] = i == 8 ? s_bright : k[i];
        }
    }
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

// Columns [x0, x1) of a turned strip, the table way
static void turned_c(const uint8_t *py, const uint8_t *pu, const uint8_t *pv, int w, board_turn_t t, int x0, int x1,
                     uint16_t *out)
{
    t.x0 = x0;
    t.x1 = x1;
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

void h264_i420_turned_to_rgb565be(const uint8_t *py, const uint8_t *pu, const uint8_t *pv, int w, int h, int rot,
                                  int first, int lines, uint16_t *out)
{
    board_turn_t t = board_turn(w, h, rot, first, lines);
    int x = t.x0;
    // a quarter turn either way with the colours as encoded: PIE, 8 columns at a time (4 for a tail), each
    // pixel pair of a column one aligned word
    if ((rot & 1) && s_sat == 100 && s_contrast == 100 && !(t.x0 & 3) && !(w & 7) && !(h & 1) && !((uintptr_t)out & 3)) {
        int ky = t.ky; // +1 (rot 1) or -1 (rot 3)
        int cols = (t.x1 - x) & ~3;
        yuv_pie_job_t j = {
            .w = w, .kx = 2 * t.kx, .step = 16 * t.kx, .cols = cols, .k = &s_pie_k[0][0], .tmp = s_pie_tmp, .swap = ky < 0,
        };
        for (int y = t.y0; y < t.y1; y += 2) {
            j.y = py + y * w + x;
            j.u = pu + (y / 2) * (w / 2) + x / 2;
            j.v = pv + (y / 2) * (w / 2) + x / 2;
            j.o = out + t.k0 + t.kx * x + ky * y + (ky < 0 ? -1 : 0);
            yuv_pie_turned(&j);
        }
        x += cols;
    }
    if (x < t.x1) {
        turned_c(py, pu, pv, w, t, x, t.x1, out);
    }
}

// Source columns [xa, xb) and rows [ya, yb) (all even) to out, pixel (x, y) at out + k0 + kx x + ky y with ky +1 or
// -1: the PIE code for columns from a multiple of 4 in steps of 4, the tables for the rest
static void block_turned(const uint8_t *py, const uint8_t *pu, const uint8_t *pv, int w, int xa, int xb, int ya, int yb,
                         uint16_t *out, int k0, int kx, int ky)
{
    board_turn_t t = {.kx = kx, .ky = ky, .k0 = k0, .x0 = xa, .x1 = xb, .y0 = ya, .y1 = yb};
    int x = xa;
    if (s_sat == 100 && s_contrast == 100 && !(xa & 3) && !(w & 7) && !((uintptr_t)out & 3)) {
        int cols = (xb - xa) & ~3;
        yuv_pie_job_t j = {
            .w = w, .kx = 2 * kx, .step = 16 * kx, .cols = cols, .k = &s_pie_k[0][0], .tmp = s_pie_tmp, .swap = ky < 0,
        };
        for (int y = ya; y < yb && cols; y += 2) {
            j.y = py + y * w + xa;
            j.u = pu + (y / 2) * (w / 2) + xa / 2;
            j.v = pv + (y / 2) * (w / 2) + xa / 2;
            j.o = out + k0 + kx * xa + ky * y + (ky < 0 ? -1 : 0);
            yuv_pie_turned(&j);
        }
        x += cols;
    }
    if (x < xb) {
        turned_c(py, pu, pv, w, t, x, xb, out);
    }
}

// Panel column c shows source row s_sy[c], panel row r source column s_sx[r] (sideways, rot 1 or 3)
static int16_t s_sy[LCD_MAX_W + 2], s_sx[512];

static void sideways_maps(int w, int h, int W, int H, int rot)
{
    static int last_w, last_h, last_W, last_H, last_rot;
    if (w != last_w || h != last_h || W != last_W || H != last_H || rot != last_rot) {
        for (int c = 0; c < H && c < LCD_MAX_W; c++) {
            s_sy[c] = (rot == 1 ? c : H - 1 - c) * h / H;
        }
        for (int r = 0; r < W && r < 512; r++) {
            s_sx[r] = (rot == 1 ? W - 1 - r : r) * w / W;
        }
        last_w = w;
        last_h = h;
        last_W = W;
        last_H = H;
        last_rot = rot;
    }
}

int h264_sideways_tile_cols(int w, int h, int W, int H, int rn, int room)
{
    if (W == w && H == h) {
        int cn = room / rn & ~1;
        return cn > H ? H : cn;
    }
    // the converted source block goes after the tile: at most this many columns and rows
    int bw = (rn * w + W - 1) / W + 8;
    for (int cn = H; cn >= 2; cn -= 2) {
        int bh = (cn * h + H - 1) / H + 4;
        if (rn * cn + bw * bh <= room) {
            return cn;
        }
    }
    return 2;
}

void h264_i420_sideways_tile(const uint8_t *py, const uint8_t *pu, const uint8_t *pv, int w, int h, int W, int H, int rot,
                             int r0, int rn, int c0, int cn, uint16_t *out, int room)
{
    if (W == w && H == h) {
        // 1:1: the tile's source block straight in, turned
        if (rot == 1) {
            int xa = w - r0 - rn, ya = c0; // panel (r, c) shows (w - 1 - r, c)
            block_turned(py, pu, pv, w, xa, xa + rn, ya, ya + cn, out, (xa + rn - 1) * cn - ya, -cn, 1);
        } else {
            int xa = r0, yb = h - c0; // panel (r, c) shows (r, h - 1 - c)
            block_turned(py, pu, pv, w, xa, xa + rn, yb - cn, yb, out, -xa * cn + yb - 1, cn, -1);
        }
        return;
    }
    sideways_maps(w, h, W, H, rot);
    int a = s_sx[r0], b = s_sx[r0 + rn - 1];
    int xa = (a < b ? a : b) & ~3, xb = ((a > b ? a : b) + 4) & ~3;
    a = s_sy[c0];
    b = s_sy[c0 + cn - 1];
    int ya = (a < b ? a : b) & ~1, yb = ((a > b ? a : b) + 2) & ~1;
    xb = xb > w ? w : xb;
    yb = yb > h ? h : yb;
    int bw = xb - xa, bh = yb - ya;
    uint16_t *blk = out + rn * cn;
    if (bw * bh > room) {
        return; // h264_sideways_tile_cols leaves room for it
    }
    // the block turned once (rot 1): source column x at blk + (xb - 1 - x) * bh, its rows in order
    block_turned(py, pu, pv, w, xa, xb, ya, yb, blk, (xb - 1) * bh - ya, -bh, 1);
#if CONFIG_CHANNELSTV_STAGE_TIMING
    int64_t t0 = esp_timer_get_time();
#endif
    // two pixels a store (cn is even); a row showing the same source column as the one before is a copy of it;
    // at 2x across, each source pixel twice
    const int16_t *sy = s_sy + c0;
    bool twice = H == 2 * h;
    for (int r = 0; r < rn; r++) {
        uint16_t *o = out + r * cn;
        if (r && s_sx[r0 + r] == s_sx[r0 + r - 1]) {
            memcpy(o, o - cn, cn * 2);
            continue;
        }
        const uint16_t *src = blk + (xb - 1 - s_sx[r0 + r]) * bh - ya;
        uint32_t *o2 = (uint32_t *)o;
        if (twice) {
            for (int c = 0; c < cn; c += 2) {
                uint32_t p = src[sy[c]];
                *o2++ = p | p << 16;
            }
        } else {
            for (int c = 0; c < cn; c += 2) {
                *o2++ = src[sy[c]] | (uint32_t)src[sy[c + 1]] << 16;
            }
        }
    }
#if CONFIG_CHANNELSTV_STAGE_TIMING
    h264_scale_us += esp_timer_get_time() - t0;
#endif
}

#if CONFIG_CHANNELSTV_STAGE_TIMING
static uint16_t ref_px(const uint8_t *py, const uint8_t *pu, const uint8_t *pv, int w, int x, int y)
{
    int u = pu[(y / 2) * (w / 2) + x / 2], v = pv[(y / 2) * (w / 2) + x / 2], c = s_y[py[y * w + x]];
    return s_r[c + s_rv[v] + CLAMP_OFF] | s_g[c + s_gu[u] + s_gv[v] + CLAMP_OFF] | s_b[c + s_bu[u] + CLAMP_OFF];
}

static int px_err(uint16_t a, uint16_t b)
{
    a = a >> 8 | a << 8;
    b = b >> 8 | b << 8;
    int dr = abs((a >> 11) - (b >> 11)), dg = abs(((a >> 5) & 63) - ((b >> 5) & 63)), db = abs((a & 31) - (b & 31));
    return dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db);
}

// The sideways picture of a made-up w x h frame at W x H (rot 1 or 3) the tile way, against the tables pixel by
// pixel (mismatches, the largest difference in a 5/6/5 channel) and its time with copying out. JSON into buf.
int h264_convcheck(char *buf, size_t len, int w, int h, int W, int H, int rot)
{
    int lw = LCD_W, room = lw * DRAW_LINES, o = 0;
    size_t fsz = w * h * 3 / 2 + 64;
    uint8_t *f = heap_caps_aligned_alloc(64, fsz, MALLOC_CAP_SPIRAM);
    uint16_t *img = heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM);
    uint16_t *strip = heap_caps_aligned_alloc(16, room * 2, MALLOC_CAP_INTERNAL);
    if (!f || !img || !strip || W > 512 || H > lw) {
        o = snprintf(buf, len, "{\"error\":\"memory or size\"}");
        goto out;
    }
    uint32_t r = 7;
    for (size_t i = 0; i < fsz; i++) {
        r = r * 1103515245u + 12345u;
        f[i] = (r >> 16) & 0xff; // the whole range, past studio levels too
    }
    const uint8_t *pu = f + w * h, *pv = pu + (w / 2) * (h / 2);
    uint16_t *b = img;
    int64_t t = esp_timer_get_time();
    for (int row = 0; row < W; row += 64) {
        int lines = W - row < 64 ? W - row : 64;
        int cn = h264_sideways_tile_cols(w, h, W, H, lines, room);
        for (int c0 = 0; c0 < H; c0 += cn) {
            int n = H - c0 < cn ? H - c0 : cn;
            h264_i420_sideways_tile(f, pu, pv, w, h, W, H, rot, row, lines, c0, n, strip, room - lines * n);
            for (int i = 0; i < lines; i++) {
                memcpy(b + (row + i) * H + c0, strip + i * n, n * 2);
            }
        }
    }
    int t_tiles = esp_timer_get_time() - t;
    int bad = 0, max = 0;
    for (int pr = 0; pr < W; pr++) {
        int sx = (rot == 1 ? W - 1 - pr : pr) * w / W;
        for (int pc = 0; pc < H; pc++) {
            int sy = (rot == 1 ? pc : H - 1 - pc) * h / H;
            uint16_t ref = ref_px(f, pu, pv, w, sx, sy);
            int e = px_err(b[pr * H + pc], ref);
            bad += e != 0;
            max = e > max ? e : max;
        }
    }
    o = snprintf(buf, len,
                 "{\"src\":\"%dx%d\",\"out\":\"%dx%d\",\"rot\":%d,\"pixels\":%d,\"tiles_us\":%d,\"bad\":%d,"
                 "\"max_lsb\":%d}",
                 w, h, W, H, rot, W * H, t_tiles, bad, max);
out:
    heap_caps_free(f);
    heap_caps_free(img);
    heap_caps_free(strip);
    return o;
}
#endif

void h264_i420_to_rgb565be(const uint8_t *yuv, int stride, int coded_h, int w, int h, uint16_t *out)
{
    const uint8_t *pu = yuv + stride * coded_h, *pv = pu + (stride / 2) * (coded_h / 2);
    h264_i420_rows_to_rgb565be(yuv, pu, pv, stride, w, h, out);
}

void h264dec_init(void)
{
    tables_init();
}

#if CONFIG_CHANNELSTV_DEC_VIDEOJET
// The output being copied into (NULL when done) and who waits for it: one decoder at a time
static uint8_t *volatile s_pending;
static TaskHandle_t volatile s_waiter;

// The visible w x h of f as compact I420 (stride w), from the top left
static void frame_copy(const vj_frame_t *f, uint8_t *out, int w, int h)
{
    uint8_t *ou = out + w * h, *ov = ou + (w / 2) * (h / 2);
    for (int y = 0; y < h; y++) {
        memcpy(out + y * w, f->y + y * f->y_stride, w);
    }
    for (int y = 0; y < h / 2; y++) {
        memcpy(ou + y * (w / 2), f->u + y * f->uv_stride, w / 2);
        memcpy(ov + y * (w / 2), f->v + y * f->uv_stride, w / 2);
    }
}

static void copier_task(void *arg)
{
    h264dec_t *d = arg;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (d->quit) {
            break;
        }
        frame_copy(&d->job, d->job_out, d->job_w, d->job_h);
        s_pending = NULL;
        TaskHandle_t w = s_waiter;
        if (w) {
            xTaskNotifyGive(w);
        }
    }
    d->copier = NULL;
    vTaskDeleteWithCaps(NULL);
}

// Waits for the copy into out (any copy with out NULL) to be done
static void copy_wait(const uint8_t *out)
{
    while (s_pending && (!out || s_pending == out)) {
        s_waiter = xTaskGetCurrentTaskHandle();
        if (s_pending && (!out || s_pending == out)) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
        }
        s_waiter = NULL;
    }
}

h264dec_t *h264dec_open(void)
{
    tables_init();
    h264dec_t *d = calloc(1, sizeof(*d));
    if (d && !(d->dec = vj_decoder_create())) {
        free(d);
        return NULL;
    }
    // below the drawer (10) and the sound (9), above the decoder (7), on either core: on core 1 alone it took a
    // quarter of it at 448x368 24 fps, which the drawing needs; stack in PSRAM, it only calls memcpy
    if (d && vj_decoder_set_hold(d->dec, true) &&
        xTaskCreatePinnedToCoreWithCaps(copier_task, "fcopy", 2048, d, 8, &d->copier, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        d->copier = NULL;
    }
    return d;
}

void h264dec_close(h264dec_t *d)
{
    if (d) {
        copy_wait(NULL);
        if (d->copier) {
            d->quit = true;
            xTaskNotifyGive(d->copier);
            while (d->copier) {
                vTaskDelay(1);
            }
        }
        vj_decoder_destroy(d->dec);
        free(d);
    }
}

void h264dec_ready(const uint8_t *out)
{
    copy_wait(out);
}

int h264dec_frame(h264dec_t *d, const uint8_t *data, int len, uint8_t *out, int w, int h, int64_t *convert_us)
{
    // VideoJet takes one access unit per call and removes its emulation-prevention bytes in
    // place: tv.c's AU buffer is writable and each AU is decoded once
    uint8_t *au = (uint8_t *)data;
    int got = 0;
    for (size_t off = 0, n; off < (size_t)len; off += n) {
        n = vj_au_length(au + off, len - off);
        if (!n) {
            break;
        }
        vj_frame_t f;
        vj_status_t st = vj_decode_au(d->dec, au + off, n, &f);
        if (st < 0) {
            return -1;
        }
        if (st != VJ_FRAME) {
            continue;
        }
        d->coded_w = f.width;
        d->coded_h = f.height;
        if (w > f.width || h > f.height) {
            return -1;
        }
        int64_t a = esp_timer_get_time();
        // the frame before this one is no longer kept after this call: its copy must be done
        copy_wait(NULL);
        if (d->copier) {
            d->job = f;
            d->job_out = out;
            d->job_w = w;
            d->job_h = h;
            s_pending = out;
            xTaskNotifyGive(d->copier);
        } else {
            frame_copy(&f, out, w, h);
        }
        if (convert_us) {
            *convert_us = esp_timer_get_time() - a;
        }
        got = 1;
    }
    return got;
}

#else
void h264dec_ready(const uint8_t *out)
{
    (void)out;
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

#endif

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
