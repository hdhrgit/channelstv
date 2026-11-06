// LVGL screens, see ui.h. Everything here runs in the LVGL task, apart from the public calls
// that take the port lock. Browsing goes over the network, so a worker task does it and the
// refresh timer shows what came back.

#include "ui.h"

#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "app.h"
#include "board.h"
#include "bridge.h"
#include "tv.h"

#define C_BG     0x0f1216
#define C_PANEL  0x1c2229
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

#define PAGE  12 // entries a page
#define DEPTH 8  // folders deep

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

static lv_font_t *f14, *f18, *f22;
static lv_obj_t *s_back, *s_head, *s_wifi, *s_batt;
static lv_obj_t *s_list;
static lv_obj_t *s_toast;
static int64_t s_toast_until;
static lv_obj_t *s_power, *s_power_arc, *s_power_lbl;
static lv_style_t s_btn_style;

// Where the screen is: a stack of folders, the top one shown a page at a time.
typedef struct {
    char id[80];
    char title[64];
    int page;
} level_t;
static EXT_RAM_BSS_ATTR level_t s_stack[DEPTH];
static int s_depth = 1; // s_stack[0] is the top ("0")
static volatile bool s_back_wanted;

// What the worker brought back for the folder shown.
typedef struct {
    char id[80];
    int start;
} request_t;
static QueueHandle_t s_requests;
static EXT_RAM_BSS_ATTR cd_entry_t s_entries[PAGE];
static volatile int s_count = -2;   // -2 loading, -1 failed
static volatile int s_total;
static volatile unsigned s_result_gen, s_shown_gen;
static unsigned s_req_gen;          // bumped by each request: older answers are dropped
static unsigned s_bridge_gen = ~0u;
static bridge_state_t s_bridge_state = (bridge_state_t)-1;

static bool s_dimmed, s_screen_off;
static battery_t s_bat;
static int64_t s_bat_at;

static lv_font_t *ttf(int size)
{
    lv_font_t *f = lv_tiny_ttf_create_data(ttf_start, ttf_end - ttf_start, size);
    if (!f) {
        return (lv_font_t *)&lv_font_montserrat_14;
    }
    // LVGL's symbols (wifi, battery, play) live in the built-in Montserrat fonts
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

static lv_obj_t *round_btn(lv_obj_t *parent, int size, const char *sym, uint32_t bg, lv_event_cb_t cb, int arg)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)arg);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
    lv_label_set_text(l, sym);
    lv_obj_center(l);
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

static void toast(const char *text, int ms)
{
    lv_label_set_text(s_toast, text);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    s_toast_until = esp_timer_get_time() + ms * 1000LL;
}

// ---------------------------------------------------------------- browsing

static void browse_task(void *arg)
{
    static EXT_RAM_BSS_ATTR cd_entry_t fresh[PAGE];
    request_t req;
    while (true) {
        xQueueReceive(s_requests, &req, portMAX_DELAY);
        unsigned gen = s_req_gen;
        if (xQueuePeek(s_requests, &req, 0)) {
            continue; // a newer one is waiting: that's the one to answer
        }
        int total = 0;
        int n = bridge_browse(req.id, req.start, PAGE, fresh, &total);
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
    request_t req;
    strlcpy(req.id, lv->id, sizeof(req.id));
    req.start = lv->page * PAGE;
    s_req_gen++;
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
    if (s_depth > 1) {
        s_depth--;
        request_page();
    }
}

// ---------------------------------------------------------------- playing

// "Tuning" over the whole screen while a stream starts. Made for each start and deleted after:
// internal RAM is tight. It takes every touch, so none reaches the list below.
static lv_obj_t *s_tune, *s_tune_secs;
static volatile bool s_play_busy; // a start (which can take seconds over the network) is under way
static char s_play_url[256], s_play_title[96];
static bool s_play_live;

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
    lv_obj_set_width(name, LCD_W - 20);
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

uint16_t *ui_banner(const char *text, int w, int h)
{
    // made and deleted before LVGL draws again, so it never shows on the screen itself
    lv_obj_t *box = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_bg_color(box, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_t *l = label(box, f14, C_ACCENT);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_update_layout(box);
    lv_draw_buf_t *snap = lv_snapshot_take(box, LV_COLOR_FORMAT_RGB565);
    lv_obj_delete(box);
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

// ---------------------------------------------------------------- the list

static void on_entry(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_count) {
        return;
    }
    const cd_entry_t *en = &s_entries[i];
    if (en->container) {
        open_folder(en);
    } else if (!s_play_busy) {
        start_play(en->url, en->title, en->live);
    }
}

static void on_page(lv_event_t *e)
{
    level_t *lv = &s_stack[s_depth - 1];
    int pages = (s_total + PAGE - 1) / PAGE;
    int p = lv->page + (int)(intptr_t)lv_event_get_user_data(e);
    lv->page = p < 0 ? pages - 1 : p >= pages ? 0 : p;
    request_page();
}

static void on_back(lv_event_t *e)
{
    go_back();
}

static void on_retry(lv_event_t *e)
{
    bridge_rescan();
    request_page();
}

static void note(const char *text, uint32_t color)
{
    lv_obj_t *l = label(s_list, f14, color);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_text(l, text);
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
        snprintf(text, sizeof(text), "No bridge found on the network.\nSet its address on the web page:\nhttp://%s/", net.ip);
        note(text, C_WARN);
        break;
    case BRIDGE_UNREACHABLE:
        snprintf(text, sizeof(text), "The bridge at %s is not reachable.\nChange it on http://%s/", bridge_override(), net.ip);
        note(text, C_WARN);
        break;
    case BRIDGE_FOUND:
        note("The bridge didn't answer.", C_WARN);
        break;
    }
    lv_obj_t *b = lv_list_add_button(s_list, LV_SYMBOL_REFRESH, "Try again");
    lv_obj_add_style(b, &s_btn_style, 0);
    lv_obj_add_event_cb(b, on_retry, LV_EVENT_CLICKED, NULL);
}

static void rebuild(void)
{
    lv_obj_clean(s_list);
    level_t *lv = &s_stack[s_depth - 1];
    lbl(s_head, s_depth == 1 ? (bridge_name()[0] ? bridge_name() : "channelstv") : lv->title);
    if (s_depth > 1) {
        lv_obj_remove_flag(s_back, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_back, LV_OBJ_FLAG_HIDDEN);
    }
    if (bridge_state() != BRIDGE_FOUND) {
        bridge_note();
        return;
    }
    if (s_count == -2) {
        note("Loading...", C_DIM);
        return;
    }
    if (s_count < 0) {
        bridge_note();
        return;
    }
    if (s_count == 0) {
        note("Nothing here.", C_DIM);
        return;
    }
    for (int i = 0; i < s_count; i++) {
        const cd_entry_t *e = &s_entries[i];
        char text[112];
        if (e->container && e->children >= 0) {
            snprintf(text, sizeof(text), "%.90s  (%d)", e->title, e->children);
        } else {
            strlcpy(text, e->title, sizeof(text));
        }
        lv_obj_t *b = lv_list_add_button(s_list, e->container ? LV_SYMBOL_DIRECTORY : e->live ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_VIDEO, text);
        lv_obj_add_style(b, &s_btn_style, 0);
        lv_label_set_long_mode(lv_obj_get_child(b, 1), LV_LABEL_LONG_DOT);
        lv_obj_add_event_cb(b, on_entry, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    int pages = (s_total + PAGE - 1) / PAGE;
    if (pages > 1) {
        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 44);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        round_btn(row, 40, LV_SYMBOL_LEFT, C_PANEL, on_page, -1);
        lv_obj_t *l = label(row, f14, C_DIM);
        lv_label_set_text_fmt(l, "%d-%d of %d", lv->page * PAGE + 1, lv->page * PAGE + s_count, s_total);
        round_btn(row, 40, LV_SYMBOL_RIGHT, C_PANEL, on_page, 1);
    }
    lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);
}

// ---------------------------------------------------------------- refresh

static const char *battery_symbol(int pct)
{
    return pct > 80 ? LV_SYMBOL_BATTERY_FULL : pct > 55 ? LV_SYMBOL_BATTERY_3
         : pct > 30 ? LV_SYMBOL_BATTERY_2 : pct > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
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
    if (s_bat.mv) {
        lbl(s_batt, s_bat.charging ? LV_SYMBOL_CHARGE : battery_symbol(s_bat.percent));
        lcolor(s_batt, s_bat.percent <= 15 && !s_bat.charging ? C_BAD : C_TEXT);
    }
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

void ui_start(void)
{
    s_requests = xQueueCreate(4, sizeof(request_t));
    strcpy(s_stack[0].id, "0");
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
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_text_color(scr, lv_color_hex(C_TEXT), 0);

    lv_style_init(&s_btn_style); // what every list button gets, once for all: internal RAM is tight
    lv_style_set_bg_color(&s_btn_style, lv_color_hex(C_PANEL));
    lv_style_set_radius(&s_btn_style, 8);
    lv_style_set_border_width(&s_btn_style, 0);
    lv_style_set_text_color(&s_btn_style, lv_color_hex(C_TEXT));
    lv_style_set_text_font(&s_btn_style, f18);
    lv_style_set_pad_ver(&s_btn_style, 10);

    // the bar: back, the folder's name, Wi-Fi and battery
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LCD_W, 32);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    s_back = lv_button_create(bar);
    lv_obj_set_size(s_back, 40, 30);
    lv_obj_align(s_back, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_set_style_bg_color(s_back, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_shadow_width(s_back, 0, 0);
    lv_obj_add_event_cb(s_back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(s_back);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_20, 0);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_center(bl);
    lv_obj_add_flag(s_back, LV_OBJ_FLAG_HIDDEN);
    s_head = label(bar, f18, C_ACCENT);
    lv_obj_set_width(s_head, LCD_W - 44 - 56);
    lv_label_set_long_mode(s_head, LV_LABEL_LONG_DOT);
    lv_obj_align(s_head, LV_ALIGN_LEFT_MID, 48, 0);
    s_wifi = label(bar, &lv_font_montserrat_14, C_DIM);
    lv_label_set_text(s_wifi, LV_SYMBOL_WIFI);
    lv_obj_align(s_wifi, LV_ALIGN_RIGHT_MID, -34, 0);
    s_batt = label(bar, &lv_font_montserrat_14, C_TEXT);
    lv_obj_set_width(s_batt, 24);
    lv_obj_set_style_text_align(s_batt, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_batt, LV_ALIGN_RIGHT_MID, -6, 0);

    s_list = lv_list_create(scr);
    lv_obj_set_size(s_list, LCD_W, LCD_H - 32);
    lv_obj_align(s_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_radius(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 6, 0);
    lv_obj_set_style_pad_row(s_list, 4, 0);

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
    lv_timer_create(refresh, 200, NULL);
    refresh(NULL);
    lvgl_port_unlock();
    board_backlight(BL_ON);
    // stack in PSRAM: it only does network I/O
    xTaskCreateWithCaps(browse_task, "browse", 6144, NULL, 3, NULL, MALLOC_CAP_SPIRAM);
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
