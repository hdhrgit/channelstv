// LVGL screens, see ui.h. Everything here runs in the LVGL task, apart from the public calls
// that take the port lock. Browsing goes over the network, so a worker task does it and the
// refresh timer shows what came back.
//
// Made for fingers on a 1.54" screen: four rows a page, each the full width and 50 px tall,
// under a slim heading. Swipe up / down for the next / previous page, swipe right (or tap the
// heading's left end) to go back, tap a row to open or play it. A long press on Live TV opens a
// number pad. One object over the whole screen takes every touch and works out what it was from
// the points: a swipe moves over SWIPE_PX within SWIPE_MS, a tap less than TAP_PX.

#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs.h"

#include "app.h"
#include "board.h"
#include "bridge.h"
#include "tv.h"

#define C_BG     0x0f1216
#define C_PANEL  0x1c2229
#define C_PRESS  0x2c3a48
#define C_TEXT   0xe8eaed
#define C_DIM    0x8a939c
#define C_ACCENT 0x4fc3f7
#define C_GOOD   0x66bb6a
#define C_WARN   0xffb74d
#define C_BAD    0xef5350

#define BL_ON        80
#define BL_DIM       10
#define DIM_AFTER_MS 60000  // on battery, the screen dims after this long untouched ...
#define OFF_AFTER_MS 300000 // ... and goes dark after this (a touch or a key brings it back)

#define HEAD_H   32
#define ROWS     4                          // rows a page
#define ROW_H    ((LCD_H - HEAD_H) / ROWS)  // 52
#define BACK_W   60                         // the heading's left end goes back
#define DEPTH    8                          // folders deep
#define SWIPE_PX 40
#define SWIPE_MS 600
#define TAP_PX   20
#define OVERLAY_MS 4000                     // the playback controls hide after this
#define SETTINGS_ID "#settings"
#define NVS_NS   "channelstv"

// LVGL's heap (CONFIG_LV_USE_CUSTOM_MALLOC): PSRAM. Through plain malloc its small blocks (widgets,
// labels, and the glyph caches of every font size) all went to internal RAM and grew with every
// new title shown, until playback couldn't start its tasks.
void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) { return NULL; }
void lv_mem_remove_pool(lv_mem_pool_t pool) {}
void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) {}
lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }

void *lv_malloc_core(size_t size)
{
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    return heap_caps_realloc_prefer(p, new_size, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void lv_free_core(void *p)
{
    heap_caps_free(p);
}

extern const uint8_t ttf_start[] asm("_binary_DejaVuSans_ttf_start");
extern const uint8_t ttf_end[] asm("_binary_DejaVuSans_ttf_end");

typedef enum { VIEW_LIST, VIEW_PAD, VIEW_CAL } view_t;
typedef enum { G_NONE, G_TAP, G_LONG, G_UP, G_DOWN, G_LEFT, G_RIGHT } gesture_t;

static lv_font_t *f14, *f18, *f22, *f28;
static lv_obj_t *s_head_back, *s_head, *s_pageno, *s_wifi;
static lv_obj_t *s_row[ROWS], *s_row_icon[ROWS], *s_row_text[ROWS];
static lv_obj_t *s_note;       // a message in place of the rows
static lv_obj_t *s_pad;        // the number pad's grid
static lv_obj_t *s_cal;        // the touch test's target
static lv_obj_t *s_catch;      // over everything: takes every touch
static lv_obj_t *s_toast;
static int64_t s_toast_until;
static lv_obj_t *s_power, *s_power_arc, *s_power_lbl;
static view_t s_view = VIEW_LIST;

// Where the screen is: a stack of folders, the top one shown a page at a time.
typedef struct {
    char id[80];
    char title[64];
    int page;
} level_t;
static EXT_RAM_BSS_ATTR level_t s_stack[DEPTH];
static int s_depth = 1; // s_stack[0] is the top ("0")
static volatile bool s_back_wanted;

// What the worker brought back for the page shown.
typedef struct {
    char id[80];
    int start;
} request_t;
static QueueHandle_t s_requests;
static EXT_RAM_BSS_ATTR cd_entry_t s_entries[ROWS];
static volatile int s_count = -2;   // -2 loading, -1 failed
static volatile int s_total;        // entries in the folder, the settings row included at the top
static volatile unsigned s_result_gen, s_shown_gen;
static volatile unsigned s_req_gen; // bumped by each request: older answers are dropped
static unsigned s_bridge_gen = ~0u;
static bridge_state_t s_bridge_state = (bridge_state_t)-1;

// the touch now under way
static int s_t_x0, s_t_y0, s_t_x, s_t_y;
static int64_t s_t_t0;
static bool s_t_long;   // a long press was taken: its release is nothing more
static int s_pressed_row = -1;

static char s_digits[12]; // typed on the number pad
static int s_cal_step;
static int s_cal_raw[5][2];
static const int s_cal_at[5][2] = {{30, 30}, {210, 30}, {210, 210}, {30, 210}, {120, 120}};

static bool s_dimmed, s_screen_off;
static battery_t s_bat;
static int64_t s_bat_at;

static void rebuild(void);

static lv_font_t *ttf(int size)
{
    lv_font_t *f = lv_tiny_ttf_create_data(ttf_start, ttf_end - ttf_start, size);
    if (!f) {
        return (lv_font_t *)&lv_font_montserrat_14;
    }
    // LVGL's symbols (wifi, folder, play) live in the built-in Montserrat fonts
    f->fallback = size >= 20 ? &lv_font_montserrat_20 : &lv_font_montserrat_14;
    return f;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t bg)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return b;
}

// LVGL repaints a label on every set, even to the same text: only touch it when it changed.
static void lbl(lv_obj_t *o, const char *text)
{
    if (strcmp(lv_label_get_text(o), text) != 0) {
        lv_label_set_text(o, text);
    }
}

static void lcolor(lv_obj_t *o, uint32_t hex)
{
    lv_color_t c = lv_color_hex(hex);
    if (!lv_color_eq(lv_obj_get_style_text_color(o, LV_PART_MAIN), c)) {
        lv_obj_set_style_text_color(o, c, 0);
    }
}

static void show(lv_obj_t *o, bool on)
{
    if (on == lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
        on ? lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

static void toast(const char *text, int ms)
{
    lv_label_set_text(s_toast, text);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    s_toast_until = esp_timer_get_time() + ms * 1000LL;
}

// ---------------------------------------------------------------- browsing

static bool at_settings(void)
{
    return !strcmp(s_stack[s_depth - 1].id, SETTINGS_ID);
}

static void settings_rows(void)
{
    static const char *const titles[] = {"Touch test", "Picture", "Find the bridge again", "About"};
    for (int i = 0; i < ROWS; i++) {
        memset(&s_entries[i], 0, sizeof(s_entries[i]));
        snprintf(s_entries[i].id, sizeof(s_entries[i].id), "#set%d", i);
        strlcpy(s_entries[i].title, titles[i], sizeof(s_entries[i].title));
    }
    snprintf(s_entries[1].title, sizeof(s_entries[1].title), "Picture: %s",
             !strcmp(bridge_profile(), "esp32_4x3") ? "4:3 centre" : "16:9");
    s_count = ROWS;
    s_total = ROWS;
}

// The top folder lists the bridge's folders and, after them, Settings.
static void browse_task(void *arg)
{
    static EXT_RAM_BSS_ATTR cd_entry_t fresh[ROWS];
    request_t req;
    while (true) {
        xQueueReceive(s_requests, &req, portMAX_DELAY);
        unsigned gen = s_req_gen;
        if (xQueuePeek(s_requests, &req, 0)) {
            continue; // a newer one is waiting: that's the one to answer
        }
        int total = 0;
        int n = bridge_browse(req.id, req.start, ROWS, fresh, &total);
        bool top = !strcmp(req.id, "0");
        if (top && n >= 0 && req.start + n >= total && n < ROWS) {
            cd_entry_t *e = &fresh[n++];
            memset(e, 0, sizeof(*e));
            strcpy(e->id, SETTINGS_ID);
            strcpy(e->title, "Settings");
            e->container = true;
            e->children = -1;
        }
        if (top && n >= 0) {
            total++;
        }
        if (gen != s_req_gen) {
            continue; // the screen moved on meanwhile
        }
        if (lvgl_port_lock(1000)) {
            if (n > 0) {
                memcpy(s_entries, fresh, n * sizeof(fresh[0]));
            }
            s_count = n;
            s_total = total;
            s_result_gen++;
            lvgl_port_unlock();
        }
    }
}

// Asks for the page of the folder on top of the stack.
static void request_page(void)
{
    level_t *lv = &s_stack[s_depth - 1];
    s_req_gen++;
    if (at_settings()) {
        settings_rows();
        s_result_gen++;
        return;
    }
    request_t req;
    strlcpy(req.id, lv->id, sizeof(req.id));
    req.start = lv->page * ROWS;
    s_count = -2;
    s_result_gen++;
    xQueueSend(s_requests, &req, 0);
}

static void open_folder(const cd_entry_t *e)
{
    if (s_depth >= DEPTH) {
        return;
    }
    level_t *lv = &s_stack[s_depth++];
    strlcpy(lv->id, e->id, sizeof(lv->id));
    strlcpy(lv->title, e->title, sizeof(lv->title));
    lv->page = 0;
    request_page();
}

static void go_back(void)
{
    if (s_view != VIEW_LIST) {
        s_view = VIEW_LIST;
        s_result_gen++;
    } else if (s_depth > 1) {
        s_depth--;
        request_page();
    }
}

static void turn_page(int dir)
{
    level_t *lv = &s_stack[s_depth - 1];
    int pages = (s_total + ROWS - 1) / ROWS;
    if (pages <= 1 || s_count < 0) {
        return;
    }
    int p = lv->page + dir;
    lv->page = p < 0 ? pages - 1 : p >= pages ? 0 : p;
    request_page();
}

// ---------------------------------------------------------------- playing

// "Tuning" over the whole screen while a stream starts. Made for each start and deleted after:
// internal RAM is tight. It takes every touch, so none reaches the list below.
static lv_obj_t *s_tune, *s_tune_secs;
static volatile bool s_play_busy; // a start (which can take seconds over the network) is under way
static char s_play_url[256], s_play_title[96];
static bool s_play_live;
// what plays, for channel up / down: its folder and place in it
static char s_now_folder[80];
static int s_now_index = -1, s_now_total;
// the playback controls: a 3x3 grid drawn over the picture, and the item's details
static uint16_t *s_ov_grid, *s_ov_info;
static volatile int64_t s_ov_until;
static volatile const uint16_t *s_ov_shown;
static QueueHandle_t s_ctl;

static void on_tune_tap(lv_event_t *e)
{
    tv_stop();
    lv_label_set_text(s_tune_secs, "Cancelling...");
}

static void tuning_show(const char *title)
{
    ui_tuning_end();
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    s_toast_until = 0;
    s_tune = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_tune);
    lv_obj_set_size(s_tune, LCD_W, LCD_H);
    lv_obj_set_style_bg_color(s_tune, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(s_tune, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_tune, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_tune, on_tune_tap, LV_EVENT_CLICKED, NULL);
    lv_obj_t *spin = lv_spinner_create(s_tune);
    lv_obj_set_size(spin, 64, 64);
    lv_obj_align(spin, LV_ALIGN_CENTER, 0, -40);
    lv_obj_remove_flag(spin, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(spin, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spin, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spin, lv_color_hex(C_PANEL), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spin, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_t *name = label(s_tune, f18, C_TEXT);
    lv_obj_set_size(name, LCD_W - 20, 24);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_label_set_text(name, title);
    lv_obj_align(name, LV_ALIGN_CENTER, 0, 18);
    s_tune_secs = label(s_tune, f18, C_ACCENT);
    lv_label_set_text(s_tune_secs, "Tuning...");
    lv_obj_align(s_tune_secs, LV_ALIGN_CENTER, 0, 46);
    lv_obj_t *hint = label(s_tune, f14, C_DIM);
    lv_label_set_text(hint, "Tap to cancel");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -14);
}

static void tuning_seconds(int s)
{
    if (s_tune && s > 0 && !tv_stopping()) {
        lv_label_set_text_fmt(s_tune_secs, "Tuning... %d s", s);
    }
}

// An object made off to the side, as the panel takes it (RGB565, bytes swapped), in PSRAM.
// Deleted before LVGL draws again, so it never shows on the screen itself.
static uint16_t *render(lv_obj_t *obj, int w, int h)
{
    lv_obj_update_layout(obj);
    lv_draw_buf_t *snap = lv_snapshot_take(obj, LV_COLOR_FORMAT_RGB565);
    lv_obj_delete(obj);
    uint16_t *out = snap ? heap_caps_calloc(w * h, 2, MALLOC_CAP_SPIRAM) : NULL;
    if (out) {
        int sw = snap->header.w < w ? snap->header.w : w, sh = snap->header.h < h ? snap->header.h : h;
        for (int y = 0; y < sh; y++) {
            const uint16_t *row = (const uint16_t *)(snap->data + y * snap->header.stride);
            for (int x = 0; x < sw; x++) {
                out[y * w + x] = row[x] >> 8 | row[x] << 8;
            }
        }
    }
    if (snap) {
        lv_draw_buf_destroy(snap);
    }
    return out;
}

uint16_t *ui_banner(const char *text, int w, int h)
{
    lv_obj_t *b = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_t *l = label(b, f14, C_ACCENT);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return render(b, w, h);
}

// The 3x3 controls over the picture, and the item's details: made before each playback (the
// picture holds LVGL's lock while it plays).
static void make_overlays(const char *title, bool live)
{
    static const char *const cells[9] = {
        LV_SYMBOL_DOWN "\nCH -", LV_SYMBOL_LIST "\nInfo", LV_SYMBOL_UP "\nCH +",
        LV_SYMBOL_MINUS "\nVOL", LV_SYMBOL_STOP "\nStop", LV_SYMBOL_PLUS "\nVOL",
        LV_SYMBOL_LEFT "\nBack", "", LV_SYMBOL_CLOSE "\nHide"};
    heap_caps_free(s_ov_grid);
    heap_caps_free(s_ov_info);
    lv_obj_t *g = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(g);
    lv_obj_set_size(g, LCD_W, LCD_H);
    lv_obj_set_style_bg_color(g, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
    for (int i = 0; i < 9; i++) {
        bool ch = (i == 0 || i == 2) && live;
        bool on = i != 7 && (ch || (i != 0 && i != 2) || s_now_index >= 0);
        lv_obj_t *c = box(g, (i % 3) * 80 + 2, (i / 3) * 80 + 2, 76, 76, i == 4 ? 0x5a1f1f : i == 7 ? 0x000000 : C_PANEL);
        lv_obj_t *l = label(c, i == 7 ? f14 : f18, on ? C_TEXT : C_DIM);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        if (i == 7) {
            lv_obj_set_width(l, 76);
            lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
            lv_label_set_text(l, title);
        } else {
            lv_label_set_text(l, cells[i]);
        }
        lv_obj_center(l);
    }
    s_ov_grid = render(g, LCD_W, LCD_H);
    lv_obj_t *in = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(in);
    lv_obj_set_size(in, LCD_W, LCD_H);
    lv_obj_set_style_bg_color(in, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(in, LV_OPA_COVER, 0);
    lv_obj_t *t = label(in, f22, C_ACCENT);
    lv_obj_set_width(t, LCD_W - 20);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(t, title);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -30);
    lv_obj_t *d = label(in, f14, C_DIM);
    char text[96];
    if (s_now_index >= 0) {
        snprintf(text, sizeof(text), "%s, %d of %d\n\nTap to go back to the picture", live ? "Live" : "Recording",
                 s_now_index + 1, s_now_total);
    } else {
        snprintf(text, sizeof(text), "%s\n\nTap to go back to the picture", live ? "Live" : "Recording");
    }
    lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(d, text);
    lv_obj_align(d, LV_ALIGN_CENTER, 0, 50);
    s_ov_info = render(in, LCD_W, LCD_H);
}

void ui_tuning_end(void)
{
    if (s_tune) {
        lv_obj_delete(s_tune);
        s_tune = s_tune_secs = NULL;
    }
}

// The DVR can take 15 s and more to start a channel. "Tuning" covers the menu until the first
// picture (a tap on it cancels); then the picture takes the screen.
static void play_task(void *arg)
{
    int64_t t0 = esp_timer_get_time();
    if (lvgl_port_lock(500)) {
        make_overlays(s_play_title, s_play_live);
        lvgl_port_unlock();
    }
    s_ov_shown = NULL;
    bool started = tv_play(s_play_url, s_play_title, s_play_live);
    if (started && lvgl_port_lock(500)) {
        tuning_show(s_play_title);
        lvgl_port_unlock();
    }
    int shown_s = -1;
    while (started && tv_playing() && !tv_first_picture_ms()) {
        int s = (esp_timer_get_time() - t0) / 1000000;
        // the picture takes the lock for good at its first frame: only try briefly
        if (s != shown_s && lvgl_port_lock(10)) {
            if (!tv_first_picture_ms()) {
                tuning_seconds(s);
            }
            shown_s = s;
            lvgl_port_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (lvgl_port_lock(tv_playing() ? 10 : 500)) {
        ui_tuning_end(); // playback did it already if it got to a picture
        lvgl_port_unlock();
    }
    if (!started || (!tv_playing() && !tv_first_picture_ms() && strcmp(tv_end_reason(), "stopped"))) {
        char text[160];
        snprintf(text, sizeof(text), "%s\n%s", tv_busy() ? "The bridge is busy" : "Can't play this",
                 tv_busy() ? "it runs all the conversions it allows" : tv_error());
        if (lvgl_port_lock(500)) {
            toast(text, 4000);
            lvgl_port_unlock();
        }
        if (!tv_busy()) {
            bridge_rescan(); // it may have moved
        }
    }
    s_play_busy = false;
    vTaskDelete(NULL);
}

static bool start_play(const char *url, const char *title, bool live)
{
    if (s_play_busy) {
        return false;
    }
    s_play_busy = true;
    strlcpy(s_play_url, url, sizeof(s_play_url));
    strlcpy(s_play_title, title, sizeof(s_play_title));
    s_play_live = live;
    if (xTaskCreate(play_task, "play", 6144, NULL, 4, NULL) != pdPASS) {
        s_play_busy = false;
        return false;
    }
    return true;
}

static void play_entry(const char *folder, int index, int total, const cd_entry_t *e)
{
    strlcpy(s_now_folder, folder, sizeof(s_now_folder));
    s_now_index = index;
    s_now_total = total;
    start_play(e->url, e->title, e->live);
}

// ---------------------------------------------------------------- playback controls

typedef struct {
    tv_touch_t what;
    int x, y;
} ctl_t;

// From the draw task: hand it on, the work happens in ctl_task.
static void on_tv_touch(tv_touch_t what, int x, int y)
{
    ctl_t c = {what, x, y};
    xQueueSend(s_ctl, &c, 0);
}

static void overlay(const uint16_t *img)
{
    s_ov_shown = img;
    s_ov_until = img ? esp_timer_get_time() + OVERLAY_MS * 1000LL : 0;
    tv_overlay(img);
}

// The next or the previous item in the folder of what plays (channel up / down).
static void step_item(int dir)
{
    if (s_now_index < 0 || s_now_total <= 1 || s_play_busy) {
        return;
    }
    int at = (s_now_index + dir + s_now_total) % s_now_total, total = 0;
    cd_entry_t e;
    for (int tries = 0; tries < 4; tries++) { // folders between the items are skipped
        if (bridge_browse(s_now_folder, at, 1, &e, &total) != 1) {
            return;
        }
        if (!e.container) {
            overlay(NULL);
            play_entry(s_now_folder, at, total, &e);
            return;
        }
        at = (at + dir + total) % total;
    }
}

static void ctl_task(void *arg)
{
    ctl_t c;
    while (true) {
        if (!xQueueReceive(s_ctl, &c, pdMS_TO_TICKS(200))) {
            if (s_ov_shown && esp_timer_get_time() > s_ov_until) {
                overlay(NULL);
            }
            continue;
        }
        ui_wake();
        if (c.what == TV_SWIPE_UP || c.what == TV_SWIPE_DOWN) {
            step_item(c.what == TV_SWIPE_UP ? 1 : -1);
            continue;
        }
        if (c.what == TV_SWIPE_RIGHT) {
            overlay(NULL);
            tv_stop();
            continue;
        }
        if (c.what != TV_TAP) {
            continue;
        }
        if (s_ov_shown != s_ov_grid || !s_ov_grid) {
            overlay(s_ov_shown || !s_ov_grid ? NULL : s_ov_grid); // a tap on the picture or the details
            continue;
        }
        s_ov_until = esp_timer_get_time() + OVERLAY_MS * 1000LL;
        int cell = (c.y / 80 > 2 ? 2 : c.y / 80) * 3 + (c.x / 80 > 2 ? 2 : c.x / 80);
        switch (cell) {
        case 0: step_item(-1); break;
        case 1: overlay(s_ov_info); break;
        case 2: step_item(1); break;
        case 3: app_set_volume(app_volume() - 5); break;
        case 4: overlay(NULL); tv_stop(); break;
        case 5: app_set_volume(app_volume() + 5); break;
        case 6: overlay(NULL); tv_stop(); s_back_wanted = true; break;
        case 8: overlay(NULL); break;
        default: break;
        }
    }
}

// ---------------------------------------------------------------- the number pad

static const char s_pad_keys[12] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', '.', '0', '>'};

static bool in_live(void)
{
    return !strcmp(s_stack[s_depth - 1].id, "live");
}

// Looks for channel s_digits in the Live TV folder and plays it.
static void jump_task(void *arg)
{
    char want[96];
    snprintf(want, sizeof(want), "live/%s", s_digits);
    static EXT_RAM_BSS_ATTR cd_entry_t e[20];
    int total = 1, found = -1, at = 0;
    for (int start = 0; start < total && found < 0; start += 20) {
        int n = bridge_browse("live", start, 20, e, &total);
        if (n <= 0) {
            break;
        }
        for (int i = 0; i < n && found < 0; i++) {
            if (!strcmp(e[i].id, want)) {
                found = start + i;
                at = i;
            }
        }
    }
    if (found >= 0) {
        play_entry("live", found, total, &e[at]);
    } else if (lvgl_port_lock(500)) {
        char text[64];
        snprintf(text, sizeof(text), "No channel %s", s_digits);
        toast(text, 2500);
        lvgl_port_unlock();
    }
    vTaskDelete(NULL);
}

static void pad_key(char k)
{
    size_t n = strlen(s_digits);
    if (k == '>') {
        if (n) {
            s_view = VIEW_LIST;
            s_result_gen++;
            xTaskCreateWithCaps(jump_task, "jump", 6144, NULL, 4, NULL, MALLOC_CAP_SPIRAM);
        }
        return;
    }
    if (n + 1 < sizeof(s_digits) - 4) {
        s_digits[n] = k;
        s_digits[n + 1] = '\0';
    }
    s_result_gen++;
}

// ---------------------------------------------------------------- touch test

static void cal_load(void)
{
    nvs_handle_t h;
    touch_cal_t c;
    size_t len = sizeof(c);
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "touchcal", &c, &len) == ESP_OK && len == sizeof(c)) {
            board_touch_set_cal(&c);
        }
        nvs_close(h);
    }
}

// Least squares t = a * r + b; returns the RMS error.
static float fit(const float *r, const float *t, int n, float *a, float *b)
{
    float sr = 0, st = 0, srr = 0, srt = 0;
    for (int i = 0; i < n; i++) {
        sr += r[i];
        st += t[i];
        srr += r[i] * r[i];
        srt += r[i] * t[i];
    }
    float d = n * srr - sr * sr;
    if (fabsf(d) < 1e-3f) {
        *a = 1;
        *b = 0;
        return 1e9f;
    }
    *a = (n * srt - sr * st) / d;
    *b = (st - *a * sr) / n;
    float e = 0;
    for (int i = 0; i < n; i++) {
        float x = *a * r[i] + *b - t[i];
        e += x * x;
    }
    return sqrtf(e / n);
}

// Five taps on five dots, raw points against where the dots are: the axis swap and the scale
// and offset of each axis that fit best. Saved if they fit within 12 px.
static void cal_finish(void)
{
    float rx[5], ry[5], tx[5], ty[5];
    for (int i = 0; i < 5; i++) {
        rx[i] = s_cal_raw[i][0];
        ry[i] = s_cal_raw[i][1];
        tx[i] = s_cal_at[i][0];
        ty[i] = s_cal_at[i][1];
    }
    touch_cal_t c[2] = {{.swap = false}, {.swap = true}};
    float err[2];
    err[0] = fmaxf(fit(rx, tx, 5, &c[0].ax, &c[0].bx), fit(ry, ty, 5, &c[0].ay, &c[0].by));
    err[1] = fmaxf(fit(ry, tx, 5, &c[1].ax, &c[1].bx), fit(rx, ty, 5, &c[1].ay, &c[1].by));
    int best = err[1] < err[0];
    char text[96];
    if (err[best] <= 12) {
        board_touch_set_cal(&c[best]);
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_blob(h, "touchcal", &c[best], sizeof(c[best]));
            nvs_commit(h);
            nvs_close(h);
        }
        snprintf(text, sizeof(text), "Touch calibrated\nwithin %.0f px", err[best]);
    } else {
        snprintf(text, sizeof(text), "The taps didn't line up\n(%.0f px off): try again", err[best]);
    }
    toast(text, 3000);
    s_view = VIEW_LIST;
    s_result_gen++;
}

// ---------------------------------------------------------------- what a touch does

static void activate_row(int i)
{
    if (i < 0 || i >= s_count) {
        return;
    }
    const cd_entry_t *e = &s_entries[i];
    if (at_settings()) {
        net_t net;
        char text[200], base[96];
        switch (i) {
        case 0:
            s_view = VIEW_CAL;
            s_cal_step = 0;
            break;
        case 1:
            bridge_set_profile(!strcmp(bridge_profile(), "esp32_4x3") ? "esp32" : "esp32_4x3");
            settings_rows();
            break;
        case 2:
            bridge_rescan();
            toast("Searching...", 1500);
            break;
        case 3:
            app_net(&net);
            bridge_base(base, sizeof(base));
            snprintf(text, sizeof(text), "channelstv %s\n%s\nBridge: %s\n%s", CHANNELSTV_VERSION, net.ip,
                     base[0] ? bridge_found_by() : "none", bridge_name());
            toast(text, 5000);
            break;
        }
        s_result_gen++;
        return;
    }
    if (e->container) {
        open_folder(e);
    } else if (!s_play_busy) {
        play_entry(s_stack[s_depth - 1].id, s_stack[s_depth - 1].page * ROWS + i, s_total, e);
    }
}

static void on_gesture(gesture_t g, int x, int y)
{
    lv_display_trigger_activity(NULL);
    if (s_view == VIEW_CAL) {
        if (g == G_RIGHT) {
            go_back();
            return;
        }
        if (g != G_TAP) {
            return;
        }
        // the raw point of this tap: the one the touch began with
        int rx, ry;
        if (!board_touch_raw(&rx, &ry)) {
            return;
        }
        s_cal_raw[s_cal_step][0] = rx;
        s_cal_raw[s_cal_step][1] = ry;
        if (++s_cal_step == 5) {
            cal_finish();
        }
        s_result_gen++;
        return;
    }
    if (s_view == VIEW_PAD) {
        if (g == G_RIGHT || (g == G_TAP && y < HEAD_H && x < BACK_W)) {
            go_back();
        } else if (g == G_LEFT || (g == G_TAP && y < HEAD_H)) {
            size_t n = strlen(s_digits); // backspace
            if (n) {
                s_digits[n - 1] = '\0';
            }
            s_result_gen++;
        } else if (g == G_TAP) {
            int r = (y - HEAD_H) / ROW_H, c = x / 80;
            pad_key(s_pad_keys[(r > 3 ? 3 : r) * 3 + (c > 2 ? 2 : c)]);
        }
        return;
    }
    switch (g) {
    case G_UP: turn_page(1); break;
    case G_DOWN: turn_page(-1); break;
    case G_RIGHT: go_back(); break;
    case G_LONG:
        if (in_live() && bridge_state() == BRIDGE_FOUND) {
            s_view = VIEW_PAD;
            s_digits[0] = '\0';
            s_result_gen++;
        }
        break;
    case G_TAP:
        if (y < HEAD_H) {
            if (x < BACK_W) {
                go_back();
            }
        } else if (s_note && !lv_obj_has_flag(s_note, LV_OBJ_FLAG_HIDDEN)) {
            bridge_rescan(); // "Try again"
            request_page();
        } else {
            activate_row((y - HEAD_H) / ROW_H);
        }
        break;
    default: break;
    }
}

static void row_highlight(int i)
{
    if (s_pressed_row >= 0 && s_pressed_row < ROWS) {
        lv_obj_set_style_bg_color(s_row[s_pressed_row], lv_color_hex(C_PANEL), 0);
    }
    s_pressed_row = i;
    if (i >= 0 && i < ROWS && s_view == VIEW_LIST && i < s_count) {
        lv_obj_set_style_bg_color(s_row[i], lv_color_hex(C_PRESS), 0);
    }
}

static void on_touch(lv_event_t *e)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        s_t_x0 = s_t_x = p.x;
        s_t_y0 = s_t_y = p.y;
        s_t_t0 = esp_timer_get_time();
        s_t_long = false;
        row_highlight(p.y >= HEAD_H ? (p.y - HEAD_H) / ROW_H : -1);
        break;
    case LV_EVENT_PRESSING:
        s_t_x = p.x;
        s_t_y = p.y;
        if (abs(s_t_x - s_t_x0) > TAP_PX || abs(s_t_y - s_t_y0) > TAP_PX) {
            row_highlight(-1); // a swipe, not a tap on the row
        }
        break;
    case LV_EVENT_LONG_PRESSED:
        if (abs(s_t_x - s_t_x0) < TAP_PX && abs(s_t_y - s_t_y0) < TAP_PX) {
            s_t_long = true;
            row_highlight(-1);
            on_gesture(G_LONG, s_t_x0, s_t_y0);
        }
        break;
    case LV_EVENT_RELEASED: {
        row_highlight(-1);
        if (s_t_long) {
            break; // the long press was the gesture
        }
        int dx = s_t_x - s_t_x0, dy = s_t_y - s_t_y0, adx = abs(dx), ady = abs(dy);
        int ms = (esp_timer_get_time() - s_t_t0) / 1000;
        if ((adx > SWIPE_PX || ady > SWIPE_PX) && ms < SWIPE_MS) {
            on_gesture(adx > ady ? (dx > 0 ? G_RIGHT : G_LEFT) : (dy > 0 ? G_DOWN : G_UP), s_t_x0, s_t_y0);
        } else if (adx < TAP_PX && ady < TAP_PX) {
            on_gesture(G_TAP, s_t_x0, s_t_y0);
        }
        break;
    }
    default: break;
    }
}

// ---------------------------------------------------------------- the screen

static void note(const char *text, uint32_t color)
{
    lbl(s_note, text);
    lcolor(s_note, color);
    show(s_note, true);
}

// What the bridge search says, when there is nothing to list.
static void bridge_note(void)
{
    net_t net;
    app_net(&net);
    char text[200];
    switch (bridge_state()) {
    case BRIDGE_WAITING:
        snprintf(text, sizeof(text), "Connecting to Wi-Fi%s%s...", net.ssid[0] ? " " : "", net.ssid);
        note(text, C_DIM);
        return;
    case BRIDGE_SEARCHING:
        note("Looking for the bridge...", C_DIM);
        return;
    case BRIDGE_NOT_FOUND:
        snprintf(text, sizeof(text), "No bridge found on the network.\nSet its address on the web page:\nhttp://%s/\n\nTap to try again", net.ip);
        break;
    case BRIDGE_UNREACHABLE:
        snprintf(text, sizeof(text), "The bridge at %s is not reachable.\nChange it on http://%s/\n\nTap to try again",
                 bridge_override(), net.ip);
        break;
    case BRIDGE_FOUND:
        snprintf(text, sizeof(text), "The bridge didn't answer.\n\nTap to try again");
        break;
    }
    note(text, C_WARN);
}

static void rows_hidden(void)
{
    for (int i = 0; i < ROWS; i++) {
        show(s_row[i], false);
    }
}

static void rebuild(void)
{
    level_t *lv = &s_stack[s_depth - 1];
    show(s_note, false);
    show(s_pad, s_view == VIEW_PAD);
    show(s_cal, s_view == VIEW_CAL);
    show(s_head_back, s_depth > 1 || s_view != VIEW_LIST);
    lbl(s_pageno, "");
    if (s_view == VIEW_CAL) {
        rows_hidden();
        lbl(s_head, "Touch test");
        lv_obj_set_pos(s_cal, s_cal_at[s_cal_step % 5][0] - 14, s_cal_at[s_cal_step % 5][1] - 14);
        char text[96];
        snprintf(text, sizeof(text), "Tap the centre of the dot\n%d of 5", s_cal_step + 1);
        note(text, C_DIM);
        lv_obj_align(s_note, LV_ALIGN_CENTER, 0, s_cal_step == 4 ? 50 : 0);
        return;
    }
    lv_obj_align(s_note, LV_ALIGN_TOP_LEFT, 8, HEAD_H + 8);
    if (s_view == VIEW_PAD) {
        rows_hidden();
        char text[48];
        snprintf(text, sizeof(text), "Channel %s_", s_digits);
        lbl(s_head, text);
        return;
    }
    lbl(s_head, s_depth == 1 ? (bridge_name()[0] ? bridge_name() : "channelstv") : lv->title);
    if (bridge_state() != BRIDGE_FOUND && !at_settings()) {
        rows_hidden();
        bridge_note();
        return;
    }
    if (s_count == -2) {
        rows_hidden();
        note("Loading...", C_DIM);
        return;
    }
    if (s_count < 0) {
        rows_hidden();
        bridge_note();
        return;
    }
    if (s_count == 0) {
        rows_hidden();
        note("Nothing here.", C_DIM);
        return;
    }
    for (int i = 0; i < ROWS; i++) {
        if (i >= s_count) {
            show(s_row[i], false);
            continue;
        }
        const cd_entry_t *e = &s_entries[i];
        char text[112];
        if (e->container && e->children >= 0) {
            snprintf(text, sizeof(text), "%.90s (%d)", e->title, e->children);
        } else {
            strlcpy(text, e->title, sizeof(text));
        }
        lbl(s_row_text[i], text);
        lbl(s_row_icon[i], at_settings() ? LV_SYMBOL_SETTINGS
                           : !strcmp(e->id, SETTINGS_ID) ? LV_SYMBOL_SETTINGS
                           : e->container ? LV_SYMBOL_DIRECTORY : e->live ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_VIDEO);
        lv_obj_set_style_bg_color(s_row[i], lv_color_hex(C_PANEL), 0);
        show(s_row[i], true);
    }
    int pages = (s_total + ROWS - 1) / ROWS;
    if (pages > 1) {
        char p[24];
        snprintf(p, sizeof(p), "%d/%d", lv->page + 1, pages);
        lbl(s_pageno, p);
    }
}

static void refresh(lv_timer_t *timer)
{
    int64_t now = esp_timer_get_time();
    if (now - s_bat_at > 2000000 || !s_bat_at) {
        board_battery(&s_bat);
        s_bat_at = now;
    }
    // on battery, dim after a minute and go dark after five; any touch or key brings it back
    int64_t inactive = lv_display_get_inactive_time(NULL);
    bool on_battery = !s_bat.usb;
    bool dim = on_battery && inactive > DIM_AFTER_MS, off = on_battery && inactive > OFF_AFTER_MS;
    if (dim != s_dimmed || off != s_screen_off) {
        s_dimmed = dim;
        s_screen_off = off;
        board_backlight(off ? 0 : dim ? BL_DIM : BL_ON);
    }
    if (s_back_wanted) {
        s_back_wanted = false;
        go_back();
    }
    net_t net;
    app_net(&net);
    lcolor(s_wifi, !net.connected ? C_BAD : net.rssi > -60 ? C_GOOD : net.rssi > -72 ? C_WARN : C_BAD);
    // the bridge found (again) or lost: start over from the top
    if (s_bridge_gen != bridge_gen()) {
        s_bridge_gen = bridge_gen();
        s_depth = 1;
        s_stack[0].page = 0;
        if (bridge_state() == BRIDGE_FOUND) {
            request_page();
        }
    }
    if (s_bridge_state != bridge_state()) {
        s_bridge_state = bridge_state();
        s_shown_gen = ~s_result_gen; // the note on the screen changes
    }
    if (s_shown_gen != s_result_gen) {
        s_shown_gen = s_result_gen;
        rebuild();
    }
    if (s_toast_until && now > s_toast_until) {
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
        s_toast_until = 0;
    }
}

static void build_power_overlay(void)
{
    s_power = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_power);
    lv_obj_set_size(s_power, LCD_W, LCD_H);
    lv_obj_set_style_bg_color(s_power, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_power, LV_OPA_90, 0);
    s_power_arc = lv_arc_create(s_power);
    lv_obj_set_size(s_power_arc, 120, 120);
    lv_obj_align(s_power_arc, LV_ALIGN_CENTER, 0, -14);
    lv_arc_set_rotation(s_power_arc, 270);
    lv_arc_set_bg_angles(s_power_arc, 0, 360);
    lv_arc_set_range(s_power_arc, 0, 3000);
    lv_obj_remove_style(s_power_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_power_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_power_arc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_power_arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_power_arc, lv_color_hex(0x2a323b), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_power_arc, lv_color_hex(C_TEXT), LV_PART_INDICATOR);
    lv_obj_t *icon = lv_label_create(s_power_arc);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(C_TEXT), 0);
    lv_label_set_text(icon, LV_SYMBOL_POWER);
    lv_obj_center(icon);
    s_power_lbl = label(s_power, f18, C_TEXT);
    lv_obj_align(s_power_lbl, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_obj_add_flag(s_power, LV_OBJ_FLAG_HIDDEN);
}

static void build_screen(lv_obj_t *scr)
{
    // the heading: back at the left end, the folder's name, the page, Wi-Fi
    s_head_back = label(scr, &lv_font_montserrat_20, C_ACCENT);
    lv_label_set_text(s_head_back, LV_SYMBOL_LEFT);
    lv_obj_align(s_head_back, LV_ALIGN_TOP_LEFT, 8, 6);
    s_head = label(scr, f18, C_ACCENT);
    lv_obj_set_size(s_head, LCD_W - 34 - 70, 24);
    lv_label_set_long_mode(s_head, LV_LABEL_LONG_DOT);
    lv_obj_align(s_head, LV_ALIGN_TOP_LEFT, 34, 7);
    s_pageno = label(scr, f14, C_DIM);
    lv_obj_set_width(s_pageno, 50);
    lv_obj_set_style_text_align(s_pageno, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_pageno, LV_ALIGN_TOP_RIGHT, -26, 9);
    s_wifi = label(scr, &lv_font_montserrat_14, C_DIM);
    lv_label_set_text(s_wifi, LV_SYMBOL_WIFI);
    lv_obj_align(s_wifi, LV_ALIGN_TOP_RIGHT, -6, 9);

    for (int i = 0; i < ROWS; i++) {
        s_row[i] = box(scr, 4, HEAD_H + i * ROW_H + 1, LCD_W - 8, ROW_H - 2, C_PANEL);
        s_row_icon[i] = label(s_row[i], &lv_font_montserrat_20, C_ACCENT);
        lv_obj_align(s_row_icon[i], LV_ALIGN_LEFT_MID, 10, 0);
        s_row_text[i] = label(s_row[i], f22, C_TEXT);
        lv_obj_set_size(s_row_text[i], LCD_W - 8 - 46, 28); // one line, cut short with "..."
        lv_label_set_long_mode(s_row_text[i], LV_LABEL_LONG_DOT);
        lv_obj_align(s_row_text[i], LV_ALIGN_LEFT_MID, 40, 0);
        lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_note = label(scr, f18, C_DIM);
    lv_obj_set_width(s_note, LCD_W - 16);
    lv_label_set_long_mode(s_note, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_note, LV_ALIGN_TOP_LEFT, 8, HEAD_H + 8);

    // the number pad: 3 x 4 keys under the heading
    s_pad = lv_obj_create(scr);
    lv_obj_remove_style_all(s_pad);
    lv_obj_set_pos(s_pad, 0, HEAD_H);
    lv_obj_set_size(s_pad, LCD_W, LCD_H - HEAD_H);
    lv_obj_remove_flag(s_pad, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 12; i++) {
        lv_obj_t *k = box(s_pad, (i % 3) * 80 + 3, (i / 3) * ROW_H + 2, 74, ROW_H - 4, i == 11 ? 0x0277bd : C_PANEL);
        lv_obj_t *l = label(k, i == 11 ? &lv_font_montserrat_28 : f28, C_TEXT);
        char t[4] = {s_pad_keys[i], 0};
        lv_label_set_text(l, i == 11 ? LV_SYMBOL_PLAY : t);
        lv_obj_center(l);
    }
    lv_obj_add_flag(s_pad, LV_OBJ_FLAG_HIDDEN);

    // the touch test's dot
    s_cal = lv_obj_create(scr);
    lv_obj_remove_style_all(s_cal);
    lv_obj_set_size(s_cal, 28, 28);
    lv_obj_set_style_radius(s_cal, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_cal, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_cal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_cal, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_border_width(s_cal, 3, 0);
    lv_obj_remove_flag(s_cal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_cal, LV_OBJ_FLAG_HIDDEN);

    // over everything on the screen: every touch comes here
    s_catch = lv_obj_create(scr);
    lv_obj_remove_style_all(s_catch);
    lv_obj_set_size(s_catch, LCD_W, LCD_H);
    lv_obj_add_flag(s_catch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_catch, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_catch, on_touch, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_catch, on_touch, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_catch, on_touch, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(s_catch, on_touch, LV_EVENT_RELEASED, NULL);
}

void ui_start(void)
{
    s_requests = xQueueCreate(4, sizeof(request_t));
    s_ctl = xQueueCreate(8, sizeof(ctl_t));
    strcpy(s_stack[0].id, "0");
    cal_load();
    lvgl_port_cfg_t port = ESP_LVGL_PORT_INIT_CONFIG();
    port.task_affinity = 0;
    port.task_stack = 8192;
    lvgl_port_init(&port);
    lvgl_port_display_cfg_t disp = {
        .io_handle = board_lcd_io(),
        .panel_handle = board_lcd_panel(),
        .buffer_size = LCD_W * 20, // internal DMA RAM, x2; playback borrows them too
        .double_buffer = true,
        .hres = LCD_W,
        .vres = LCD_H,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {.buff_dma = true, .swap_bytes = true},
    };
    lv_display_t *d = lvgl_port_add_disp(&disp);
    if (board_touch()) {
        lvgl_port_touch_cfg_t touch = {.disp = d, .handle = board_touch()};
        lvgl_port_add_touch(&touch);
    }

    lvgl_port_lock(0);
    f14 = ttf(14);
    f18 = ttf(18);
    f22 = ttf(22);
    f28 = ttf(28);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_text_color(scr, lv_color_hex(C_TEXT), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    build_screen(scr);

    s_toast = lv_label_create(lv_layer_top());
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_toast, 12, 0);
    lv_obj_set_style_pad_all(s_toast, 14, 0);
    lv_obj_set_style_text_font(s_toast, f22, 0);
    lv_obj_set_style_text_color(s_toast, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_align(s_toast, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_max_width(s_toast, LCD_W - 16, 0); // long errors wrap instead of running off
    lv_label_set_long_mode(s_toast, LV_LABEL_LONG_WRAP);
    lv_obj_center(s_toast);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    build_power_overlay();
    lv_timer_create(refresh, 100, NULL);
    refresh(NULL);
    lvgl_port_unlock();
    board_backlight(BL_ON);
    tv_set_touch_handler(on_tv_touch);
    // stack in PSRAM: it only does network I/O
    xTaskCreateWithCaps(browse_task, "browse", 6144, NULL, 3, NULL, MALLOC_CAP_SPIRAM);
    // internal stack: the volume it sets is saved to NVS (flash), which a task with its stack
    // in PSRAM must not do
    xTaskCreate(ctl_task, "tvctl", 4096, NULL, 4, NULL);
}

bool ui_touch(const char *gesture, int x, int y)
{
    static const struct { const char *name; gesture_t g; tv_touch_t t; } names[] = {
        {"tap", G_TAP, TV_TAP}, {"long", G_LONG, TV_NONE}, {"up", G_UP, TV_SWIPE_UP},
        {"down", G_DOWN, TV_SWIPE_DOWN}, {"left", G_LEFT, TV_SWIPE_LEFT}, {"right", G_RIGHT, TV_SWIPE_RIGHT}};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(gesture, names[i].name)) {
            continue;
        }
        if (tv_playing() && tv_first_picture_ms()) {
            on_tv_touch(names[i].t, x, y); // the picture has the screen
            return true;
        }
        if (!lvgl_port_lock(500)) {
            return false;
        }
        on_gesture(names[i].g, x, y);
        lvgl_port_unlock();
        return true;
    }
    return false;
}

void ui_back(void)
{
    s_back_wanted = true; // the refresh timer does it, in the LVGL task
}

void ui_backlight_full(void)
{
    s_dimmed = s_screen_off = false; // refresh() dims it again once the screen has sat idle long enough
    board_backlight(BL_ON);
}

void ui_power_hold(int held_ms)
{
    if (!lvgl_port_lock(100)) {
        return; // playback has the screen
    }
    if (held_ms <= 0) {
        lv_obj_add_flag(s_power, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_arc_set_value(s_power_arc, held_ms);
        lv_label_set_text(s_power_lbl, held_ms >= 3000 ? "Powering off" : "Keep holding to power off");
        lv_obj_remove_flag(s_power, LV_OBJ_FLAG_HIDDEN);
        lv_display_trigger_activity(NULL);
        ui_backlight_full();
    }
    lvgl_port_unlock();
}

bool ui_play(const char *url, const char *title, bool live)
{
    s_now_index = -1; // from the web page: no folder to step through
    return start_play(url, title, live);
}

void ui_wake(void)
{
    if (lvgl_port_lock(100)) {
        lv_display_trigger_activity(NULL);
        lvgl_port_unlock();
    }
}

void ui_show_volume(int volume)
{
    if (lvgl_port_lock(100)) {
        lv_label_set_text_fmt(s_toast, "%s  %d%%", volume ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE, volume);
        lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
        s_toast_until = esp_timer_get_time() + 1500000;
        lvgl_port_unlock();
    }
}
