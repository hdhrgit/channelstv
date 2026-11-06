// TV playback, see tv.h.
//
// Three tasks besides the drawer: the stream's fetcher (stream.c, core 1) reads the stream into
// a ring; the demuxer here (core 1) takes TS packets from it, writes the sound (ADTS) into the
// player and queues the H.264 access units with their PTS; the decoder (tv_video_task, core 0)
// maps each PTS onto the sound's clock and hands frames to the draw task (core 1), which sends
// each one to the panel (LVGL paused) when the sound's clock reaches it. The compressed queue
// is deep (seconds) so the demuxer never has to wait for the picture while the sound runs dry,
// nor the other way round.
//
// Timestamps belong to an "epoch": a stretch of the stream whose PTS run on continuously.
// A new connection or a PTS jump starts the next one, which waits for a keyframe and is
// placed where the sound written so far ends.

#include "tv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "src/display/lv_display_private.h"

#include "board.h"
#include "h264dec.h"
#include "player.h"
#include "stream.h"
#include "ts.h"
#include "ui.h"

#define MAX_FRAME_BYTES (128 * 1024)

static const char *TAG = "tv";

static volatile bool s_playing;
static volatile bool s_stop;
static const char *volatile s_why = ""; // why playback ended: end, tap, stopped, speaker taken, error
// Playback tasks don't delete themselves: vTaskDeleteWithCaps(NULL) hands that to a helper task
// and, with the next channel starting right away, the TCB (internal RAM) was sometimes never
// freed, ~350 B per channel switch. They clear their flag and wait; whoever waited deletes them.
static TaskHandle_t s_main_task, s_draw_task, s_demux_task;

static void task_done(volatile bool *running)
{
    *running = false;
    vTaskSuspend(NULL);
}

static void task_reap(TaskHandle_t *t)
{
    if (*t) {
        vTaskDeleteWithCaps(*t);
        *t = NULL;
    }
}

static uint32_t s_audio_gen;
static char s_title[96];
#define UNDECODABLE 25 // decode errors before the first picture that end playback
static char s_error[128];
static volatile int64_t s_first_pts = -1; // of the stream's first picture ...
static volatile int32_t s_media_ms;       // ... and how far past it the decoder is
static void (*volatile s_hook)(void *);   // tv_call_locked(): run by the decoder, which holds LVGL
static void *volatile s_hook_arg;
static int s_shown, s_dropped;
static float s_decode_ms, s_draw_ms, s_fps;
static volatile bool s_live;         // a live channel: catching up and trimming keep it near the live edge
static volatile int32_t s_frame_us = 41667; // a frame's time, from the PTS: the draw task's "late" limit
static volatile bool s_preroll_done; // the first frame is up and the decoder is ahead: the sound may start
static volatile int s_pend_ms;       // sound held back so far
static int64_t s_start_us;           // when this playback was asked for
static int s_first_ms;               // from then to the first picture on the screen
static int64_t s_late_sum_ms;        // shown frames: how far the clock was past their time
static int s_late_max_ms;

// Where a finger is now, on the screen; false if none.
static bool touch_point(int *x, int *y)
{
    esp_lcd_touch_handle_t tp = board_touch();
    if (!tp) {
        return false;
    }
    esp_lcd_touch_point_data_t pt;
    uint8_t n = 0;
    esp_lcd_touch_read_data(tp);
    if (esp_lcd_touch_get_data(tp, &pt, &n, 1) != ESP_OK || n == 0) {
        return false;
    }
    *x = pt.x;
    *y = pt.y;
    return true;
}

// Taps and swipes from the points the draw task samples. A swipe moves more than SWIPE_PX
// within SWIPE_MS; a tap moves less than TAP_PX; anything else (a slow drag) is nothing.
#define SWIPE_PX 40
#define SWIPE_MS 600
#define TAP_PX   20
static tv_touch_fn s_touch_fn;
typedef struct {
    bool down, armed;
    int x0, y0, x, y;
    int64_t t0;
} gesture_t;

static void gesture_step(gesture_t *g)
{
    int x, y;
    bool down = touch_point(&x, &y);
    if (down && !g->down) {
        g->x0 = g->x = x;
        g->y0 = g->y = y;
        g->t0 = esp_timer_get_time();
    } else if (down) {
        g->x = x;
        g->y = y;
    } else if (g->down && g->armed) {
        int dx = g->x - g->x0, dy = g->y - g->y0, adx = abs(dx), ady = abs(dy);
        int ms = (esp_timer_get_time() - g->t0) / 1000;
        tv_touch_t what = TV_NONE;
        if ((adx > SWIPE_PX || ady > SWIPE_PX) && ms < SWIPE_MS) {
            what = adx > ady ? (dx > 0 ? TV_SWIPE_RIGHT : TV_SWIPE_LEFT) : (dy > 0 ? TV_SWIPE_DOWN : TV_SWIPE_UP);
        } else if (adx < TAP_PX && ady < TAP_PX) {
            what = TV_TAP;
        }
        if (what != TV_NONE) {
            if (s_touch_fn) {
                s_touch_fn(what, g->x0, g->y0);
            } else if (what == TV_TAP) {
                s_why = "tap";
                s_stop = true;
            }
        }
    }
    if (!down) {
        g->armed = true; // the finger that started playback has to lift first
    }
    g->down = down;
}

#define STRIP_LINES 20 // LVGL's draw buffers are LCD_W x 20

// LVGL's two internal-RAM draw buffers, borrowed while the picture holds the LVGL lock: internal
// RAM is too tight (Wi-Fi, the decoder) for the player to keep strips of its own.
static uint16_t *s_strip[2];
static char s_draw_err[64];

// The panel's SPI DMA can't read the PSRAM frame reliably (only the top of it arrived), so
// each frame goes out in strips copied to internal RAM. Two strips take turns: the next
// draw's window commands wait for the previous strip's pixels to finish sending.
// Converts an I420 frame (compact, Y stride w) strip by strip into the panel's RGB565 and
// sends it, centred. The conversion runs here on core 1, not in the decoder on core 0: at
// 240x192 decoding alone takes most of a frame's time.
static void draw_frame(esp_lcd_panel_handle_t panel, uint16_t *strip[2], const uint8_t *frame, int w, int h)
{
    int x = (LCD_W - w) / 2, y = (LCD_H - h) / 2;
    const uint8_t *pu = frame + w * h, *pv = pu + (w / 2) * (h / 2);
    for (int row = 0, s = 0; row < h; row += STRIP_LINES, s ^= 1) { // STRIP_LINES is even
        int lines = h - row < STRIP_LINES ? h - row : STRIP_LINES;
        h264_i420_rows_to_rgb565be(frame + row * w, pu + row / 2 * (w / 2), pv + row / 2 * (w / 2), w, w, lines, strip[s]);
        esp_err_t err = esp_lcd_panel_draw_bitmap(panel, x, y + row, x + w, y + row + lines, strip[s]);
        if (err != ESP_OK && !s_draw_err[0]) {
            snprintf(s_draw_err, sizeof(s_draw_err), "draw row %d: %s", row, esp_err_to_name(err));
        }
    }
}

// A full-screen RGB565 image (panel byte order, in PSRAM) through the internal-RAM strips.
static void draw_rgb(esp_lcd_panel_handle_t panel, uint16_t *strip[2], const uint16_t *img)
{
    for (int y = 0, k = 0; y < LCD_H; y += STRIP_LINES, k ^= 1) {
        memcpy(strip[k], img + y * LCD_W, LCD_W * STRIP_LINES * 2);
        esp_lcd_panel_draw_bitmap(panel, 0, y, LCD_W, y + STRIP_LINES, strip[k]);
    }
}

// Where playback is: the sound's position while there is sound, else the wall clock
// (carrying on from where the sound ended).
typedef struct {
    bool audio;
    int64_t t0_us, last_pos, last_at_us;
} clock_t_;

static int64_t clock_ms(clock_t_ *c)
{
    int64_t now = esp_timer_get_time();
    if (!c->audio) {
        return (now - c->t0_us) / 1000;
    }
    if (!s_preroll_done) {
        // the sound hasn't started: the clock stands still at the first picture (running it on
        // the wall clock let the picture play ahead of the sound, then drop a second of frames)
        c->last_at_us = now;
        return c->last_pos;
    }
    if (player_is_playing() && player_gen() == s_audio_gen) {
        int64_t p = player_position_ms();
        if (p != c->last_pos) {
            c->last_pos = p;
            c->last_at_us = now;
        }
        return p;
    }
    return c->last_pos + (now - c->last_at_us) / 1000;
}

// Decoded frames handed from the decoder (tv_video_task) to the drawer (draw_task).
#define FR_END  -1 // no more frames
#define FR_IDLE -2 // nothing to draw yet (waiting for the stream)
typedef struct {
    int buf;      // which s_fb, FR_END or FR_IDLE
    int w, h;
    int64_t due_ms;
} frame_t;

// Decoded frames waiting to be drawn. Keyframes take ~1.5x a frame's time to decode, so with
// only two buffers one of them cost a dropped frame every GOP (about 25 a minute); a few
// more let the decoder get ahead between keyframes and absorb them.
#define NUM_FB 5
static uint8_t *s_fb[NUM_FB]; // I420
static QueueHandle_t s_free_q, s_full_q;
static clock_t_ s_clk;
static volatile bool s_draw_running;
static volatile int s_pic_w, s_pic_h;           // the stream's picture
static volatile bool s_banner_want;             // "Catching up..." under the picture
static uint16_t *volatile s_banner;             // panel-order RGB565, LCD_W x BANNER_H, in PSRAM: the
                                                // one shown (one of the two below)
static uint16_t *s_banner_catch, *s_banner_buf; // "Catching up...", "Buffering..."
#define BANNER_H 20
static const char *volatile s_drstate = ""; // where the draw task is (debug)
static const uint16_t *volatile s_overlay;  // drawn over the whole screen instead of the picture
static volatile bool s_overlay_dirty;
// /api/screen asks the draw task for a copy of the next frame it draws (nothing happens when
// nobody asks): the I420 picture goes into s_cap, allocated on first use.
static uint8_t *s_cap;
static volatile bool s_cap_want, s_cap_done;
static volatile int s_cap_w, s_cap_h;

static void draw_task(void *arg)
{
    esp_lcd_panel_handle_t panel = board_lcd_panel();
    int64_t t0 = esp_timer_get_time(), draw_us = 0;
    gesture_t g = {0};
    bool overlay_on = false;
    frame_t fr;
    bool banner_on = false;
    uint16_t *banner_dma = NULL; // freed a frame after its last use: by then the panel has it
    int banner_age = 0;
    while ((s_drstate = "waiting", xQueueReceive(s_full_q, &fr, portMAX_DELAY)) && fr.buf != FR_END) {
        if (board_rotation_apply()) {
            s_overlay_dirty = true; // the whole screen again, the new way up
        }
        if (s_banner && !overlay_on && banner_on != s_banner_want && s_pic_h && (LCD_H - s_pic_h) / 2 >= BANNER_H) {
            if (!banner_dma) {
                banner_dma = heap_caps_malloc(LCD_W * BANNER_H * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
            }
            if (banner_dma) {
                banner_on = s_banner_want;
                if (banner_on) {
                    memcpy(banner_dma, s_banner, LCD_W * BANNER_H * 2);
                } else {
                    memset(banner_dma, 0, LCD_W * BANNER_H * 2);
                }
                // in the bar under the picture
                int y = (LCD_H + s_pic_h) / 2 + ((LCD_H - s_pic_h) / 2 - BANNER_H) / 2;
                esp_lcd_panel_draw_bitmap(panel, 0, y, LCD_W, y + BANNER_H, banner_dma);
                banner_age = 0;
            }
        } else if (banner_dma && !banner_on && ++banner_age > 2) {
            heap_caps_free(banner_dma);
            banner_dma = NULL;
        }
        if (s_overlay_dirty) {
            s_overlay_dirty = false;
            const uint16_t *ov = s_overlay;
            overlay_on = ov != NULL;
            s_drstate = "overlay";
            if (ov) {
                draw_rgb(panel, s_strip, ov);
            } else {
                memset(s_strip[0], 0, LCD_W * STRIP_LINES * 2); // black around the picture again
                for (int y = 0; y < LCD_H; y += STRIP_LINES) {
                    esp_lcd_panel_draw_bitmap(panel, 0, y, LCD_W, y + STRIP_LINES, s_strip[0]);
                }
                esp_lcd_panel_draw_bitmap(panel, 0, 0, 1, 1, s_strip[1]); // the black strip has left
                banner_on = false;
            }
        }
        if (fr.buf == FR_IDLE) {
            goto touch; // waiting for the stream: only look for a touch
        }
        int64_t now_ms = clock_ms(&s_clk);
        // the sound is held back until the first frame is up: that one has no clock to wait for
        bool first = !s_shown;
        s_drstate = "waiting for the clock";
        while (!s_stop && fr.due_ms > now_ms && !first) {
            vTaskDelay(pdMS_TO_TICKS(fr.due_ms - now_ms > 5 ? fr.due_ms - now_ms - 2 : 1));
            now_ms = clock_ms(&s_clk);
        }
        if (!s_stop && (now_ms <= fr.due_ms + s_frame_us / 1000 || first)) {
            int64_t a = esp_timer_get_time();
            s_drstate = "drawing";
            if (!overlay_on) {
                draw_frame(panel, s_strip, s_fb[fr.buf], fr.w, fr.h);
            }
            int64_t b = esp_timer_get_time();
            draw_us += b - a;
            s_shown++;
            if (s_cap_want) {
                memcpy(s_cap, s_fb[fr.buf], fr.w * fr.h * 3 / 2);
                s_cap_w = fr.w;
                s_cap_h = fr.h;
                s_cap_want = false;
                s_cap_done = true;
            }
            if (!s_first_ms) {
                s_first_ms = (b - s_start_us) / 1000;
            }
            int late = now_ms - fr.due_ms;
            s_late_sum_ms += late;
            s_late_max_ms = late > s_late_max_ms ? late : s_late_max_ms;
            s_draw_ms = draw_us / 1000.0f / s_shown;
            s_fps = s_shown * 1e6f / (b - t0);
        } else if (!s_stop) {
            s_dropped++; // decoded in time but the clock moved on while it waited
        }
        xQueueSend(s_free_q, &fr.buf, portMAX_DELAY);
    touch:
        s_drstate = "touch";
        gesture_step(&g); // every frame (~24 a second): a swipe is over in a few of them
    }
    // one more window command makes sure the last strip left its buffer
    s_drstate = "ending";
    esp_lcd_panel_draw_bitmap(panel, 0, 0, 1, 1, s_strip[0]);
    heap_caps_free(banner_dma);
    s_drstate = "ended";
    task_done(&s_draw_running);
}

// Hands the screen back to LVGL (call with its lock held). The finger that tapped the picture
// away is usually still down: LVGL must not take its lift as a tap on whatever is under it
// (that started the item under the finger again).
static void give_back_screen(void)
{
    for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i)) {
        if (lv_indev_get_type(i) == LV_INDEV_TYPE_POINTER) {
            lv_indev_reset(i, NULL);
            lv_indev_wait_release(i);
        }
    }
    lv_obj_invalidate(lv_screen_active());
    lv_display_trigger_activity(NULL); // the dim timer starts over after playback
    lvgl_port_unlock();
}

// Ends the draw task: it takes the end marker after the frames already queued. If the queue
// is full, frames nobody will draw now make room for it.
static void end_drawer(void)
{
    frame_t end = {.buf = FR_END}, old;
    while (s_draw_running && !xQueueSend(s_full_q, &end, pdMS_TO_TICKS(100))) {
        if (xQueueReceive(s_full_q, &old, 0) && old.buf >= 0) {
            xQueueSend(s_free_q, &old.buf, 0);
        }
    }
    for (int i = 0; s_draw_running; i++) {
        if (i == 300) {
            ESP_LOGW(TAG, "draw task still busy after 3 s: %s", s_drstate);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    task_reap(&s_draw_task);
}

#define AU_HEAD  12               // in front of each queued access unit: PTS, epoch, keyframe, size
#define AU_QUEUE (768 * 1024)     // ~15 s of the bridge's video, in PSRAM
#define PTS_MASK ((1LL << 33) - 1)
#define PTS_JUMP (2 * 90000)      // a PTS step bigger than this is a discontinuity

typedef struct {
    volatile bool set;
    int64_t base_pts;            // this PTS ...
    volatile int32_t base_ms;    // ... is at this point on the sound's clock
} epoch_map_t;

#define HOLD_BYTES (16 * 1024)
#define HOLD_N     96
typedef struct {
    ts_demux_t ts;
    int epoch;
    bool need_idr;               // the picture waits for a keyframe: start, lost packets, new connection
    bool idr_seen;               // this epoch's first keyframe went into the queue
    int64_t idr_pts;
    int64_t audio_end_us;        // where the sound written so far ends on the player's clock (in us:
                                 // a frame is 23.22 ms, and counting whole ms drifted 30 ms every
                                 // few seconds, which showed up as gaps that moved the picture)
    int64_t last_pts[2];
    uint8_t *pend;               // sound held back until the first frames are decoded (pre-roll)
    int pend_len;
    int64_t pend_us;             // how much sound that is
    // sound that came before this epoch's first keyframe: the muxer may put it a few hundred
    // ms ahead of the picture it belongs to, so it waits to see where the picture starts
    int64_t trim_at_us;          // audio_end_us at which the next AAC frame may be left out
    int64_t trim_carry_us;       // what was left out and not yet taken off the mapping (< 1 ms)
    uint8_t *hold;
    int hold_len, hold_n;
    struct { int64_t pts; int off, len; } held[HOLD_N];
} demux_t;

#define PREROLL_BYTES (96 * 1024) // ~12 s of the bridge's sound
#define PREBUFFER_MS  6000        // the sound starts with at least this much in hand ...
#define PREBUFFER_WAIT_MS 12000   // ... or this long after the first picture, whichever first.
                                  // Meanwhile the first picture stays up, with "Buffering..."
                                  // under it: started with only a second in hand, live TV
                                  // stalled a few times in its first minutes, until the gaps
                                  // in the bridge's delivery had built up a cushion
#define LEAD_MS 6000              // how far the demuxer may run ahead of the sound being heard:
                                  // a burst from the server then waits in the ring (and TCP)
                                  // instead of taking core 0 from the decoder

// Keeping up with the live edge. The bridge (and the DVR behind it) can hold a minute or more
// of stream that the board, playing at 1.0x, never gets through: every stall upstream added to
// it for good (84 s behind the DVR after a night on one channel). Two ways back:
//  - catching up: whenever the ring stays full for 3 minutes (the bridge has a lot waiting),
//    the fetcher drops everything waiting up to the live edge and the picture starts again from
//    the next keyframe: one jump. Not right after a start: right at the edge the bridge sends
//    its segments in bursts with gaps of up to ~14 s, and with nothing in hand the picture
//    stalled at every gap (about one stall a minute, 19-20 fps on average);
//  - trimming: while the sound and the ring hold more than TARGET_MS even at their lowest over
//    60 s, one AAC frame (23 ms) in 25 is left out and the clock moves with it, so the picture
//    plays 4 % faster too (its frames are shown a little sooner; none is lost). TARGET_MS is
//    the cushion kept against those gaps.
#define TARGET_MS 8000
#define TRIM_EVERY_US (25 * 23220) // one AAC frame left out of 25
#define LAT_SLOTS 120              // half-second samples: the lowest over 60 s
#define CATCH_UP_FULL_MS 180000
static volatile int32_t s_lat_ms, s_lat_min_ms; // what the board holds now, the lowest over 60 s
static volatile bool s_trim;
static uint32_t s_trimmed_ms, s_catchups, s_catch_skipped_ms;
static volatile int s_catch_epoch = -1;         // the picture after a catch-up starts this epoch

static stream_t *s_src;
static StreamBufferHandle_t s_au_q;
static epoch_map_t s_map[4];
static demux_t *s_dm;
static volatile bool s_demux_running;
static volatile bool s_busy;
static uint32_t s_restarts, s_epochs, s_skipped, s_audio_gaps, s_audio_late, s_audio_gap_ms;
static uint32_t s_late_decoded; // frames the decoder finished after their time (dropped for it)
static uint32_t s_starved;      // times the decoder waited for a picture from the network
static uint32_t s_cc_errors, s_packets, s_au_bytes; // copied from the demuxer, for the status
static const char *volatile s_dstate = "", *volatile s_vstate = "";
static volatile uint32_t s_ram_min;  // internal RAM low point while this channel plays
static uint32_t s_ram_at[6];         // internal RAM at each step of the start (debug)
#define RAM_AT(i) (s_ram_at[i] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL))

// a - b in 90 kHz ticks, across the 33-bit wrap (every 26.5 h)
static int64_t pts_diff(int64_t a, int64_t b)
{
    int64_t d = (a - b) & PTS_MASK;
    return d >= (1LL << 32) ? d - (1LL << 33) : d;
}

static void new_epoch(demux_t *dm)
{
    dm->epoch++;
    s_map[dm->epoch & 3].set = false;
    dm->need_idr = true;
    dm->idr_seen = false;
    dm->last_pts[0] = dm->last_pts[1] = -1;
    dm->hold_len = dm->hold_n = 0;
    s_epochs++;
}

// Microseconds of sound in a PES of ADTS frames.
static int64_t adts_us(const uint8_t *p, int len)
{
    static const int rates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    int frames = 0, rate = 44100;
    for (int i = 0; i + 7 <= len;) {
        if (p[i] != 0xff || (p[i + 1] & 0xf0) != 0xf0) {
            break;
        }
        int sr = p[i + 2] >> 2 & 0xf, flen = (p[i + 3] & 3) << 11 | p[i + 4] << 3 | p[i + 5] >> 5;
        rate = sr < 13 ? rates[sr] : rate;
        frames += (p[i + 6] & 3) + 1;
        if (flen < 7) {
            break;
        }
        i += flen;
    }
    return frames * 1024 * 1000000LL / rate;
}

static int adts_ms(const uint8_t *p, int len)
{
    return adts_us(p, len) / 1000;
}

// Sends the sound to the player; false (and the playback stops) if the player dropped it.
static bool write_sound(const uint8_t *data, int len)
{
    s_dstate = "writing sound";
    if (!player_pipe_write_until(s_audio_gen, data, len, &s_stop) && !s_stop) {
        if (player_gen() != s_audio_gen) {
            s_why = "speaker taken";
        } else {
            strcpy(s_error, "the sound stopped (stream stalled too long)");
        }
        s_stop = true;
        return false;
    }
    return true;
}

static void flush_preroll(demux_t *dm)
{
    if (dm->pend_len) {
        write_sound(dm->pend, dm->pend_len);
        dm->pend_len = 0;
        dm->pend_us = 0;
    }
}

static void on_sound(demux_t *dm, int64_t pts, const uint8_t *data, int len);

static void on_pes(void *ctx, int kind, int64_t pts, uint8_t *data, int len, bool gap)
{
    demux_t *dm = ctx;
    if (pts < 0 || len <= 0 || s_stop) {
        return;
    }
    int64_t last = dm->last_pts[kind];
    if (last >= 0 && llabs(pts_diff(pts, last)) > PTS_JUMP) {
        new_epoch(dm);
    }
    dm->last_pts[kind] = pts;
    if (kind == TS_VIDEO) {
        if (gap) {
            dm->need_idr = true; // a lost slice spoils every frame up to the next keyframe
        }
        bool key = h264_is_keyframe(data, len);
        if (dm->need_idr && !key) {
            s_skipped++;
            return;
        }
        dm->need_idr = false;
        int w = 0, h = 0;
        if (key) {
            h264_sps_size(data, len, &w, &h);
            if (!dm->idr_seen) {
                dm->idr_seen = true;
                dm->idr_pts = pts;
                if (!dm->ts.apid) { // no sound: the picture keeps the time from here
                    epoch_map_t *m = &s_map[dm->epoch & 3];
                    m->base_pts = pts;
                    m->base_ms = dm->audio_end_us / 1000;
                    m->set = true;
                }
                // the held sound from the keyframe's time on comes first
                for (int i = 0; i < dm->hold_n; i++) {
                    on_sound(dm, dm->held[i].pts, dm->hold + dm->held[i].off, dm->held[i].len);
                }
                dm->hold_len = dm->hold_n = 0;
            }
            if (w > 0 && !s_pic_w) {
                s_pic_w = w;
                s_pic_h = h;
            }
        } else if (last >= 0) {
            int ms = pts_diff(pts, last) / 90;
            if (ms >= 10 && ms <= 100) {
                s_frame_us = ms * 1000; // the draw task's "late" limit
            }
        }
        uint8_t *hd = data - AU_HEAD;
        memcpy(hd, &pts, 8);
        hd[8] = dm->epoch;
        hd[9] = key;
        hd[10] = w / 2; // 0 unless the keyframe gave its size
        hd[11] = h / 2;
        s_dstate = "queueing picture";
        while (!s_stop && !xStreamBufferSend(s_au_q, hd, len + AU_HEAD, pdMS_TO_TICKS(100))) {
            s_dstate = "picture queue full";
        }
        return;
    }
    if (!dm->idr_seen) {
        // keep the latest sound until the picture starts (oldest half dropped when full)
        if (dm->hold_n == HOLD_N || dm->hold_len + len > HOLD_BYTES) {
            int k = dm->hold_n / 2, cut = k < dm->hold_n ? dm->held[k].off : dm->hold_len;
            memmove(dm->hold, dm->hold + cut, dm->hold_len - cut);
            for (int i = k; i < dm->hold_n; i++) {
                dm->held[i - k] = dm->held[i];
                dm->held[i - k].off -= cut;
            }
            dm->hold_n -= k;
            dm->hold_len -= cut;
        }
        if (len <= HOLD_BYTES - dm->hold_len) {
            dm->held[dm->hold_n].pts = pts;
            dm->held[dm->hold_n].off = dm->hold_len;
            dm->held[dm->hold_n++].len = len;
            memcpy(dm->hold + dm->hold_len, data, len);
            dm->hold_len += len;
        }
        return;
    }
    on_sound(dm, pts, data, len);
}

// Sound from this epoch's first picture on: placed on the clock, then pre-rolled or written.
static void on_sound(demux_t *dm, int64_t pts, const uint8_t *data, int len)
{
    epoch_map_t *m = &s_map[dm->epoch & 3];
    if (!m->set) {
        // a PES may hold many ADTS frames (300 ms in ffmpeg's default muxing): drop the
        // ones from before the first picture, keep the rest
        while (len >= 7 && pts_diff(pts, dm->idr_pts) < -90 * 30) {
            int flen = (data[3] & 3) << 11 | data[4] << 3 | data[5] >> 5, ms = adts_ms(data, flen < len ? flen : len);
            if (data[0] != 0xff || flen < 7 || flen > len) {
                return;
            }
            data += flen;
            len -= flen;
            pts = (pts + ms * 90) & PTS_MASK;
        }
        if (len < 7) {
            return; // all of it from before the first picture
        }
        m->base_pts = pts;
        m->base_ms = dm->audio_end_us / 1000;
        m->set = true;
    }
    if (s_trim && s_preroll_done && dm->audio_end_us >= dm->trim_at_us && len >= 7 && data[0] == 0xff) {
        int flen = (data[3] & 3) << 11 | data[4] << 3 | data[5] >> 5;
        if (flen >= 7 && flen <= len) {
            // the first frame of this PES is left out: the rest is placed as if it had been heard,
            // so the mapping (and every picture after it) moves earlier by as much
            int64_t us = adts_us(data, flen);
            data += flen;
            len -= flen;
            pts = (pts + us * 9 / 100) & PTS_MASK;
            dm->trim_carry_us += us;
            int ms = dm->trim_carry_us / 1000;
            m->base_ms -= ms;
            dm->trim_carry_us -= ms * 1000;
            s_trimmed_ms += ms;
            dm->trim_at_us = dm->audio_end_us + TRIM_EVERY_US;
            if (len < 7) {
                return;
            }
        }
    }
    int64_t dur = adts_us(data, len), end_ms = dm->audio_end_us / 1000;
    // The player's clock counts the sound it was given, so the mapping follows what really
    // went in: lost sound moves the epoch's mapping back, sound that overlaps is dropped.
    int64_t at = m->base_ms + pts_diff(pts, m->base_pts) / 90;
    if (at > end_ms + 30) {
        m->base_ms -= at - end_ms;
        s_audio_gaps++;
        s_audio_gap_ms += at - end_ms;
    } else if (at < end_ms - 30) {
        s_audio_late++;
        return;
    }
    dm->audio_end_us += dur;
    // the sound's clock starts with its first byte: hold it until the first frames are
    // decoded, or they would all be late (15 were dropped at every start without this)
    if (!s_preroll_done && dm->pend_len + len <= PREROLL_BYTES) {
        memcpy(dm->pend + dm->pend_len, data, len);
        dm->pend_len += len;
        dm->pend_us += dur;
        s_pend_ms = dm->pend_us / 1000;
        return;
    }
    flush_preroll(dm);
    write_sound(data, len);
}

// What the board holds that hasn't been heard yet, in ms: the sound written ahead of the
// player plus the ring at the stream's rate (bytes per ms of sound, measured as it plays).
// The picture's queue runs alongside the sound, so it doesn't add to it.
static void latency(demux_t *dm, int64_t now)
{
    static int64_t next_at, rate_bytes, rate_us;
    static int32_t slots[LAT_SLOTS];
    static int slot, filled;
    static float bytes_per_ms;
    if (now < next_at) {
        return;
    }
    if (!next_at || !s_preroll_done) { // a new playback
        rate_bytes = (int64_t)dm->ts.packets * 188;
        rate_us = dm->audio_end_us;
        bytes_per_ms = 55; // ~440 kbps, until measured
        slot = filled = 0;
    }
    next_at = now + 500000;
    if (!s_preroll_done) {
        next_at = 0;
        return;
    }
    int64_t bytes = (int64_t)dm->ts.packets * 188;
    if (dm->audio_end_us - rate_us >= 5000000) {
        float r = (bytes - rate_bytes) * 1000.0f / (dm->audio_end_us - rate_us);
        bytes_per_ms = bytes_per_ms * 0.7f + r * 0.3f;
        rate_bytes = bytes;
        rate_us = dm->audio_end_us;
    }
    int32_t ahead = dm->audio_end_us / 1000 - player_position_ms();
    int32_t lat = (ahead > 0 ? ahead : 0) + (int32_t)(stream_fill(s_src) / bytes_per_ms);
    slots[slot] = lat;
    slot = (slot + 1) % LAT_SLOTS;
    filled += filled < LAT_SLOTS;
    int32_t lo = lat;
    for (int i = 0; i < filled; i++) {
        lo = slots[i] < lo ? slots[i] : lo;
    }
    s_lat_ms = lat;
    s_lat_min_ms = lo;
    s_trim = s_live && lat > TARGET_MS && lo > TARGET_MS && filled == LAT_SLOTS && !stream_catching_up(s_src);
    if (s_catch_epoch == -2) { // the catch-up just finished: what it threw away, in time
        s_catch_skipped_ms = stream_caught_up_bytes() / bytes_per_ms;
        s_catch_epoch = dm->epoch;
        filled = 0; // a fresh 60 s before trimming again
    }
}

static void tv_demux_task(void *arg)
{
    demux_t *dm = s_dm;
    uint8_t pkt[188];
    int64_t last_catch_up = esp_timer_get_time();
    while (!s_stop) {
        if (s_preroll_done) {
            flush_preroll(dm);
        }
        int64_t now = esp_timer_get_time();
        latency(dm, now);
        if (s_live && !stream_catching_up(s_src) && stream_full_ms(s_src) >= CATCH_UP_FULL_MS &&
            now - last_catch_up > CATCH_UP_FULL_MS * 1000LL) {
            last_catch_up = now;
            s_banner = s_banner_catch;
            s_banner_want = true;
            stream_catch_up(s_src);
        }
        uint32_t ram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        s_ram_min = ram < s_ram_min ? ram : s_ram_min;
        if (s_preroll_done && player_pipe_open(s_audio_gen) && dm->audio_end_us / 1000 - player_position_ms() > LEAD_MS) {
            s_dstate = "ahead";
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        s_dstate = "reading";
        int r = stream_read(s_src, pkt, 200);
        if (r == STREAM_PACKET) {
            ts_push(&dm->ts, pkt);
            if ((dm->ts.packets & 63) == 0) {
                s_cc_errors = dm->ts.cc_errors;
                s_packets = dm->ts.packets;
                s_au_bytes = xStreamBufferBytesAvailable(s_au_q);
            }
        } else if (r == STREAM_RESTART) {
            ts_reset(&dm->ts); // a new connection: new PAT/PMT, new timestamps, from a keyframe
            new_epoch(dm);
            s_restarts++;
            if (s_banner_want) {
                s_catch_epoch = dm->epoch; // a catch-up that ended in a reconnect
            }
        } else if (r == STREAM_CAUGHT_UP) {
            ts_reset(&dm->ts); // mid-packet, mid-picture: like a new connection
            new_epoch(dm);
            s_catchups++;
            s_catch_epoch = -2; // latency() fills in what was skipped
        } else if (r == STREAM_END) {
            s_why = "end";
            break;
        } else if (r == STREAM_FAILED) {
            s_busy = stream_busy(s_src);
            strlcpy(s_error, stream_error(s_src), sizeof(s_error));
            s_stop = true;
        }
    }
    player_pipe_end(s_audio_gen);
    // the connection goes now, whatever the decoder is doing
    stream_close(s_src);
    s_src = NULL;
    task_done(&s_demux_running);
}

// The sound (and with it the clock) starts once the first frame is up, the decoder is a full
// set of frames ahead (started any sooner, the start-up burst on core 0 left the decoder
// behind and a second of frames came out late) and PREBUFFER_MS of sound is in hand.
static void preroll_check(int decoded)
{
    if (s_preroll_done || decoded < NUM_FB || !s_shown) {
        return;
    }
    int64_t since_ms = (esp_timer_get_time() - s_start_us) / 1000 - s_first_ms;
    if (s_pend_ms >= PREBUFFER_MS || since_ms >= PREBUFFER_WAIT_MS) {
        s_preroll_done = true;
        s_banner_want = false; // "Buffering..." goes
        RAM_AT(5);
    }
}

// The next access unit from the queue, waiting while there is none; 0 once a recording has
// ended and the queue is empty. While the screen is the picture's, every wait lets the draw
// task look for a tap.
static size_t next_au(uint8_t *buf, bool screen)
{
    size_t n = xStreamBufferReceive(s_au_q, buf, MAX_FRAME_BYTES + AU_HEAD, 0);
    s_vstate = "waiting for a picture";
    if (!n && screen) {
        s_starved++;
    }
    while (!n && !s_stop && !(n = xStreamBufferReceive(s_au_q, buf, MAX_FRAME_BYTES + AU_HEAD, pdMS_TO_TICKS(200)))) {
        if (!s_demux_running && !xStreamBufferBytesAvailable(s_au_q)) {
            break; // the demuxer is done: nothing more is coming
        }
        if (screen) {
            frame_t idle = {.buf = FR_IDLE};
            xQueueSend(s_full_q, &idle, 0);
        }
    }
    return n;
}

static void tv_video_task(void *arg)
{
    RAM_AT(1);
    uint8_t *au = heap_caps_malloc(MAX_FRAME_BYTES + AU_HEAD, MALLOC_CAP_SPIRAM);
    for (int i = 0; i < NUM_FB; i++) {
        s_fb[i] = heap_caps_aligned_calloc(64, 1, LCD_W * LCD_H * 3 / 2, MALLOC_CAP_SPIRAM);
    }
    s_free_q = xQueueCreate(NUM_FB, sizeof(int));
    s_full_q = xQueueCreate(NUM_FB + 4, sizeof(frame_t)); // + the end marker and idle ticks
    h264dec_t *hdec = h264dec_open();
    RAM_AT(2);
    bool locked = false;
    bool fbs = s_strip[0] && s_strip[1];
    for (int i = 0; i < NUM_FB; i++) {
        fbs = fbs && s_fb[i];
    }
    if (!au || !fbs || !s_free_q || !s_full_q || !hdec) {
        snprintf(s_error, sizeof(s_error), "setup failed: buf=%d fb=%d decoder=%d", !!au, fbs, !!hdec);
        s_stop = true;
        goto done;
    }
    for (int i = 0; i < NUM_FB; i++) {
        xQueueSend(s_free_q, &i, 0);
    }
    // the screen stays the menu's until the first keyframe is here (the bridge needs a few seconds)
    size_t n = next_au(au, false);
    RAM_AT(3);
    if (s_stop) {
        goto done;
    }
    if (s_pic_w <= 0 || s_pic_h <= 0) {
        strcpy(s_error, TV_UNDECODABLE); // no size from its SPS: not a profile the decoder takes
        s_stop = true;
        goto done;
    }
    if (s_pic_w > LCD_W || s_pic_h > LCD_H) {
        snprintf(s_error, sizeof(s_error), "picture %dx%d doesn't fit the screen", s_pic_w, s_pic_h);
        s_stop = true;
        goto done;
    }
    lvgl_port_lock(0);
    locked = true;
    ui_tuning_end();
    ui_backlight_full();
    s_banner_catch = ui_banner("Catching up...", LCD_W, BANNER_H);
    s_banner_buf = ui_banner("Buffering...", LCD_W, BANNER_H);
    s_banner = s_banner_buf;
    s_banner_want = true; // until the pre-roll is done
    vTaskDelay(pdMS_TO_TICKS(20));
    memset(s_strip[0], 0, LCD_W * STRIP_LINES * 2);
    for (int y = 0; y < LCD_H; y += STRIP_LINES) {
        esp_lcd_panel_draw_bitmap(board_lcd_panel(), 0, y, LCD_W, y + STRIP_LINES, s_strip[0]);
    }
    s_clk = (clock_t_){.audio = s_dm->ts.apid != 0, .t0_us = esp_timer_get_time()};
    for (int i = 0; s_clk.audio && i < 150 && !player_pipe_open(s_audio_gen) && !s_stop; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_clk.last_at_us = esp_timer_get_time();
    s_draw_running = true;
    if (xTaskCreatePinnedToCoreWithCaps(draw_task, "tv_draw", 4096, NULL, 8, &s_draw_task, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_draw_running = false;
        strcpy(s_error, "out of RAM for the draw task");
        s_stop = true;
    }
    RAM_AT(4);
    int w = s_pic_w, h = s_pic_h, decoded = 0, undecodable = 0;
    int64_t dec_us = 0;
    for (; !s_stop && n > AU_HEAD; n = next_au(au, true)) {
        int64_t pts;
        memcpy(&pts, au, 8);
        if (s_first_pts < 0) {
            s_first_pts = pts;
        }
        s_media_ms = pts_diff(pts, s_first_pts) / 90;
        void (*hook)(void *) = s_hook;
        if (hook) {
            hook(s_hook_arg);
            s_hook = NULL;
        }
        epoch_map_t *m = &s_map[au[8] & 3];
        bool key = au[9];
        int kw = au[10] * 2, kh = au[11] * 2, len = n - AU_HEAD;
        if (s_banner_want && s_catch_epoch >= 0 && au[8] == (uint8_t)s_catch_epoch) {
            s_banner_want = false; // the first picture from the live edge
        }
        if (key && kw && (kw != w || kh != h) && kw <= LCD_W && kh <= LCD_H) {
            // a new connection with another picture size: the decoder starts afresh
            h264dec_close(hdec);
            hdec = h264dec_open();
            w = kw;
            h = kh;
            s_pic_w = w;
            s_pic_h = h;
            if (!hdec) {
                strcpy(s_error, "decoder reopen failed");
                break;
            }
        }
        // the sound places each epoch; it follows its first picture within a few packets
        for (int i = 0; !m->set && !s_stop; i++) {
            s_vstate = "waiting for the sound";
            if (i == 150) { // 3 s and still no sound: let the picture keep the time
                m->base_pts = pts;
                m->base_ms = clock_ms(&s_clk);
                m->set = true;
            }
            frame_t idle = {.buf = FR_IDLE};
            xQueueSend(s_full_q, &idle, 0);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (s_clk.audio && player_gen() != s_audio_gen) {
            s_why = "speaker taken";
            break;
        }
        frame_t fr = {.due_ms = m->base_ms + pts_diff(pts, m->base_pts) / 90};
        s_vstate = "waiting for a frame buffer";
        // all of them wait for the clock: before the start that's the pre-roll filling up
        while (!s_stop && !xQueueReceive(s_free_q, &fr.buf, pdMS_TO_TICKS(100))) {
            preroll_check(decoded);
        }
        if (s_stop) {
            break;
        }
        int64_t a = esp_timer_get_time();
        int r = h264dec_frame(hdec, au + AU_HEAD, len, s_fb[fr.buf], w, h, NULL);
        if (r <= 0) {
            if (r < 0) {
                snprintf(s_error, sizeof(s_error), "H.264 decode error (%d bytes)", len);
                s_dropped++;
                if (!decoded && ++undecodable >= UNDECODABLE) {
                    // not one picture: the stream isn't what the decoder takes (the bridge's
                    // profile); end it rather than leave a black screen
                    strcpy(s_error, TV_UNDECODABLE);
                    break;
                }
            }
            xQueueSend(s_free_q, &fr.buf, 0);
            continue;
        }
        dec_us += esp_timer_get_time() - a;
        s_decode_ms = dec_us / 1000.0f / ++decoded;
        if (s_preroll_done && s_clk.audio && player_position_ms() > fr.due_ms + s_frame_us / 1000) {
            s_late_decoded++;
        }
        fr.w = w;
        fr.h = h;
        s_vstate = "handing over a frame";
        xQueueSend(s_full_q, &fr, portMAX_DELAY);
        preroll_check(decoded);
    }
    s_vstate = "ending";
    end_drawer();
    s_vstate = "ended";
    ESP_LOGI(TAG, "%s: %d shown, %d dropped, decode %.1f ms, draw %.1f ms, %.1f fps, first picture %d ms",
             s_title, s_shown, s_dropped, s_decode_ms, s_draw_ms, s_fps, s_first_ms);

done:
    s_stop = true;
    if (!s_why[0]) {
        s_why = s_error[0] ? "error" : "stopped";
    }
    if (locked) {
        give_back_screen();
    }
    if (player_gen() == s_audio_gen) {
        player_stop(); // also lets a demuxer blocked on a full sound buffer go
    }
    while (s_demux_running) { // it closes the source (the fetcher frees it within ~1 s)
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    task_reap(&s_demux_task);
    s_vstate = "idle";
    s_banner = NULL;
    heap_caps_free(s_banner_catch);
    heap_caps_free(s_banner_buf);
    s_banner_catch = s_banner_buf = NULL;
    s_banner_want = false;
    ts_free(&s_dm->ts);
    heap_caps_free(s_dm->pend);
    heap_caps_free(s_dm->hold);
    heap_caps_free(s_dm);
    s_dm = NULL;
    vStreamBufferGenericDeleteWithCaps(s_au_q, pdTRUE);
    s_au_q = NULL;
    h264dec_close(hdec);
    heap_caps_free(au);
    for (int i = 0; i < NUM_FB; i++) {
        heap_caps_free(s_fb[i]);
        s_fb[i] = NULL;
    }
    if (s_free_q) {
        vQueueDelete(s_free_q);
        vQueueDelete(s_full_q);
        s_free_q = s_full_q = NULL;
    }
    task_done(&s_playing);
}
bool tv_play(const char *url, const char *title, bool live)
{
    if (s_playing) {
        tv_stop();
        for (int i = 0; i < 100 && s_playing; i++) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (s_playing) {
            return false;
        }
    }
    task_reap(&s_main_task);
    strlcpy(s_title, title, sizeof(s_title));
    s_live = live;
    s_frame_us = 41667; // until the PTS say otherwise
    s_start_us = esp_timer_get_time();
    s_shown = s_dropped = s_first_ms = 0;
    s_late_sum_ms = s_late_max_ms = 0;
    s_decode_ms = s_draw_ms = s_fps = 0;
    s_error[0] = s_draw_err[0] = '\0';
    s_why = "";
    s_pic_w = s_pic_h = 0;
    s_first_pts = -1;
    s_media_ms = 0;
    s_busy = false;
    s_restarts = s_epochs = s_skipped = s_audio_gaps = s_audio_late = s_audio_gap_ms = 0;
    s_late_decoded = s_starved = 0;
    s_cc_errors = s_packets = s_au_bytes = 0;
    s_lat_ms = s_lat_min_ms = 0;
    s_trim = s_banner_want = false;
    s_overlay = NULL;
    s_overlay_dirty = false;
    s_trimmed_ms = s_catchups = s_catch_skipped_ms = 0;
    s_catch_epoch = -1;
    s_preroll_done = false;
    s_pend_ms = 0;
    s_ram_min = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    memset(s_map, 0, sizeof(s_map));

    memset(s_ram_at, 0, sizeof(s_ram_at));
    RAM_AT(0);
    s_au_q = xStreamBufferGenericCreateWithCaps(AU_QUEUE, 0, pdTRUE, MALLOC_CAP_SPIRAM);
    s_dm = heap_caps_calloc(1, sizeof(demux_t), MALLOC_CAP_SPIRAM);
    if (s_dm) {
        s_dm->pend = heap_caps_malloc(PREROLL_BYTES, MALLOC_CAP_SPIRAM);
        s_dm->hold = heap_caps_malloc(HOLD_BYTES, MALLOC_CAP_SPIRAM);
    }
    if (!s_au_q || !s_dm || !s_dm->pend || !s_dm->hold || !ts_init(&s_dm->ts, MAX_FRAME_BYTES, 16384, AU_HEAD, on_pes, s_dm) ||
        !(s_src = stream_open(url, live))) {
        strcpy(s_error, "out of memory for playback");
        goto bad;
    }
    s_dm->need_idr = true; // a stream picked up anywhere starts at its next keyframe
    s_dm->last_pts[0] = s_dm->last_pts[1] = -1;
    s_stop = false;
    s_playing = true;
    s_audio_gen = player_play_pipe(s_title);
    s_demux_running = true;
    if (xTaskCreatePinnedToCoreWithCaps(tv_demux_task, "tv_demux", 6144, NULL, 6, &s_demux_task, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_demux_running = false;
    }
    if (!s_demux_running ||
        xTaskCreatePinnedToCoreWithCaps(tv_video_task, "tv_video", 8192, NULL, 7, &s_main_task, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
        strcpy(s_error, "out of RAM for the playback tasks");
        s_stop = true;
        player_stop();
        while (s_demux_running) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        task_reap(&s_demux_task);
        s_playing = false;
        goto bad;
    }
    return true;
bad:
    stream_close(s_src);
    s_src = NULL;
    if (s_dm) {
        ts_free(&s_dm->ts);
        heap_caps_free(s_dm->pend);
        heap_caps_free(s_dm->hold);
        heap_caps_free(s_dm);
        s_dm = NULL;
    }
    if (s_au_q) {
        vStreamBufferGenericDeleteWithCaps(s_au_q, pdTRUE);
        s_au_q = NULL;
    }
    return false;
}

void tv_init(void)
{
    h264dec_init();
    lv_display_t *d = lv_display_get_default();
    if (d && d->buf_1 && d->buf_2 && d->buf_1->data_size >= LCD_W * STRIP_LINES * 2) {
        s_strip[0] = (uint16_t *)d->buf_1->data;
        s_strip[1] = (uint16_t *)d->buf_2->data;
    }
}

void tv_set_touch_handler(tv_touch_fn fn)
{
    s_touch_fn = fn;
}

const uint16_t *tv_overlay_shown(void)
{
    return s_playing ? s_overlay : NULL;
}

void tv_overlay(const uint16_t *img)
{
    if (img != s_overlay) {
        s_overlay = img;
        s_overlay_dirty = true;
    }
}

void tv_stop(void)
{
    if (s_playing && !s_why[0]) {
        s_why = "stopped";
    }
    s_stop = true;
}

bool tv_playing(void)
{
    return s_playing;
}

bool tv_stopping(void)
{
    return s_playing && s_stop;
}

bool tv_busy(void)
{
    return s_busy;
}

const char *tv_end_reason(void)
{
    return s_why;
}

const char *tv_error(void)
{
    return s_error[0] ? s_error : s_draw_err;
}

bool tv_seek_ignored(void)
{
    return s_playing && s_src && stream_seek_ignored(s_src);
}

int tv_position_ms(void)
{
    return s_media_ms;
}

bool tv_call_locked(void (*fn)(void *), void *arg)
{
    if (!s_playing || !s_first_ms || s_hook) {
        return false;
    }
    s_hook_arg = arg;
    s_hook = fn;
    for (int i = 0; i < 50 && s_hook; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_hook) {
        s_hook = NULL; // the stream has stalled: not now
        return false;
    }
    return true;
}

int tv_first_picture_ms(void)
{
    return s_first_ms;
}

const uint8_t *tv_capture(int *w, int *h, int timeout_ms)
{
    if (!s_playing || !s_shown || !s_draw_running) {
        return NULL;
    }
    if (!s_cap && !(s_cap = heap_caps_malloc(LCD_W * LCD_H * 3 / 2, MALLOC_CAP_SPIRAM))) {
        return NULL;
    }
    s_cap_done = false;
    s_cap_want = true;
    for (int i = 0; i < timeout_ms / 10 && !s_cap_done && s_playing; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_cap_want = false;
    if (!s_cap_done) {
        return NULL;
    }
    *w = s_cap_w;
    *h = s_cap_h;
    return s_cap;
}

static void json_escape(char *out, size_t len, const char *in)
{
    size_t o = 0;
    for (; *in && o + 2 < len; in++) {
        if (*in == '"' || *in == '\\') {
            out[o++] = '\\';
        }
        out[o++] = (unsigned char)*in < 0x20 ? ' ' : *in;
    }
    out[o] = '\0';
}

void tv_status_json(char *out, size_t len)
{
    char src[384], title[200], err[260];
    stream_status_json(src, sizeof(src));
    json_escape(title, sizeof(title), s_title);
    json_escape(err, sizeof(err), tv_error());
    snprintf(out, len,
             "{\"title\":\"%s\",\"playing\":%s,\"live\":%s,\"busy\":%s,\"width\":%d,\"height\":%d,\"fps\":%.1f,"
             "\"shown\":%d,\"dropped\":%d,\"decode_ms\":%.1f,\"draw_ms\":%.1f,\"first_picture_ms\":%d,"
             "\"late_avg_ms\":%.1f,\"late_max_ms\":%d,\"restarts\":%u,\"epochs\":%u,\"skipped_to_idr\":%u,"
             "\"audio_gaps\":%u,\"audio_gap_ms\":%u,\"audio_late\":%u,\"late_decoded\":%u,\"starved\":%u,"
             "\"cc_errors\":%u,\"packets\":%u,\"au_queue\":%u,\"latency_ms\":%d,\"latency_min_ms\":%d,"
             "\"trimming\":%s,\"trimmed_ms\":%u,\"catchups\":%u,\"catchup_skipped_ms\":%u,\"catching_up\":%s,"
             "\"demux\":\"%s\",\"decoder\":\"%s\",\"drawer\":\"%s\",\"ram_now\":%u,\"ram_min\":%u,"
             "\"ended\":\"%s\",\"error\":\"%s\",\"source\":%s}",
             title, s_playing ? "true" : "false", s_live ? "true" : "false", s_busy ? "true" : "false", s_pic_w, s_pic_h,
             s_fps, s_shown, s_dropped, s_decode_ms, s_draw_ms, s_first_ms, s_shown ? (float)s_late_sum_ms / s_shown : 0.0f,
             s_late_max_ms, (unsigned)s_restarts, (unsigned)s_epochs, (unsigned)s_skipped, (unsigned)s_audio_gaps,
             (unsigned)s_audio_gap_ms, (unsigned)s_audio_late, (unsigned)s_late_decoded, (unsigned)s_starved,
             (unsigned)s_cc_errors, (unsigned)s_packets, (unsigned)s_au_bytes, (int)s_lat_ms, (int)s_lat_min_ms,
             s_trim ? "true" : "false", (unsigned)s_trimmed_ms, (unsigned)s_catchups, (unsigned)s_catch_skipped_ms,
             s_banner_want ? "true" : "false", s_dstate, s_vstate, s_drstate,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)s_ram_min, s_why, err, src);
}
