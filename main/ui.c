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
#include "esp_wifi.h"
#include "esp_app_desc.h"
#include "cJSON.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "src/display/lv_display_private.h"
#include "nvs.h"

#include "app.h"
#include "board.h"
#include "bridge.h"
#include "rotate.h"
#include "setup.h"
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

#define BL_DIM       10
// The backlight's level (Settings, saved), and on battery how long untouched before the screen
// dims (seconds) and goes dark (minutes); 0: never.
static int s_bright = 80, s_dim_s = 60, s_off_min = 5;
static int s_rows = 4, s_head_h = 32;
// When a page was asked for while the one before stays on the screen (0: none). Blanking the
// rows for the bridge's answer made the screen flash dark on every page turn; "Loading..."
// only shows when the answer is slow.
static volatile int64_t s_loading_us;
#define LOADING_SHOW_US 500000

// Sizes are laid out for the 1.54's 240 px wide panel; PX() scales them to this panel's width,
// so a bigger, denser panel shows the same layout at about the same size on the glass.
#define PX(v)    ((v) * LCD_W / 240)
#define HEAD_H   s_head_h                   // PX(32); on a tall panel as tall as a row
#define HEAD_DY  ((HEAD_H - PX(32)) / 2)    // what centres the heading's parts in a taller one
#define ROWS_MAX 6
#define ROWS     s_rows                     // rows a page: 4 on the 1.54, 6 on the taller AMOLED
#define ROW_H    ((LCD_H - HEAD_H) / ROWS)  // 52 on the 1.54
#define PR(v)    ((v) * ROW_H / 52)         // inside a row: scaled to the row's height
#define PAD_H    ((LCD_H - HEAD_H) / 4)     // the number pad's 4 rows of keys
#define BACK_W   PX(60)                     // the heading's left end goes back
#define DEPTH    8                          // folders deep
#define SWIPE_PX PX(40)
#define SWIPE_MS 600
#define TAP_PX   PX(20)
#define OVERLAY_MS 4000                     // the playback controls hide after this
#define SEEK_STEP_S  30                     // a tap on the seek keys ...
#define SEEK_IDLE_MS 800                    // ... applied once the taps stop for this long
#define RESUME_MIN_S 30                     // a recording resumes from where it was left, unless
#define RESUME_END_S 60                     // that's in its first or last this many seconds
#define RESUME_EVERY_US 15000000LL          // where it is, saved this often while it plays
#define QR_PX    PX(116)                    // the setup hotspot's QR code, quiet zone not included
#define SETTINGS_ID "#settings"
#define VERSION_ID  "#version"
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
static lv_obj_t *s_head_back, *s_head, *s_pageno, *s_wifi, *s_batt;
static bool s_batt_shown;
static lv_obj_t *s_row[ROWS_MAX], *s_row_icon[ROWS_MAX], *s_row_text[ROWS_MAX];
static lv_obj_t *s_row_cap[ROWS_MAX]; // the Version page's names, over their values in s_row_text
static lv_obj_t *s_row_new[ROWS_MAX]; // a dot on a recording nobody has watched (upnp:playbackCount 0)
static lv_obj_t *s_note;       // a message in place of the rows
static lv_obj_t *s_pad;        // the number pad's grid
static lv_obj_t *s_cal;        // the touch test's target
static lv_obj_t *s_qr;         // the setup hotspot's QR code
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
    int total; // its items, from its last answer (0: not known yet)
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
static EXT_RAM_BSS_ATTR cd_entry_t s_entries[ROWS_MAX];
static volatile int s_count = -2;   // -2 loading, -1 failed
static volatile int s_total;        // entries in the folder, the settings row included at the top
static volatile unsigned s_result_gen, s_shown_gen;
static volatile unsigned s_req_gen; // bumped by each request: older answers are dropped
static unsigned s_bridge_gen = ~0u;
static unsigned s_setup_gen = ~0u;
static bridge_state_t s_bridge_state = (bridge_state_t)-1;

// the touch now under way
static int s_t_x0, s_t_y0, s_t_x, s_t_y;
static int64_t s_t_t0;
static int s_swipe_dy, s_swipe_ms; // the last vertical swipe, for the fling (0: from the web API)
static bool s_t_long;   // a long press was taken: its release is nothing more
static int s_pressed_row = -1;

static char s_digits[12]; // typed on the number pad
static int s_cal_step;
static int s_cal_raw[5][2];
static const int s_cal_at[5][2] = {{30, 30}, {210, 30}, {210, 210}, {30, 210}, {120, 120}}; // on 240 x 240

// Where touch-test dot i is on this panel: the same places, stretched to its size.
static void cal_at(int i, int *x, int *y)
{
    *x = s_cal_at[i][0] * LCD_W / 240;
    *y = s_cal_at[i][1] * LCD_H / 240;
}

static bool s_dimmed, s_screen_off;
static bool s_api_touch; // the gesture under way came from /api/touch, not a finger
static battery_t s_bat;
static int64_t s_bat_at;

static void rebuild(void);
static void head_fit(void);

// Montserrat (LVGL's symbols) in the size nearest v px scaled: 14, 20 or 28 are built in
static const lv_font_t *sym(int v)
{
    int px = PX(v);
    return px >= 26 ? &lv_font_montserrat_28 : px >= 18 ? &lv_font_montserrat_20 : &lv_font_montserrat_14;
}

static lv_font_t *ttf(int size)
{
    lv_font_t *f = lv_tiny_ttf_create_data(ttf_start, ttf_end - ttf_start, size);
    if (!f) {
        return (lv_font_t *)&lv_font_montserrat_14;
    }
    // LVGL's symbols (wifi, folder, play) live in the built-in Montserrat fonts
    f->fallback = size >= 26 ? &lv_font_montserrat_28 : size >= 20 ? &lv_font_montserrat_20 : &lv_font_montserrat_14;
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
    lv_obj_set_style_radius(b, PX(8), 0);
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
    // a wrapped label sized to its content comes out a line short: long ones get the full width
    lv_obj_set_width(s_toast, strlen(text) > 20 ? LCD_W - PX(16) : LV_SIZE_CONTENT);
    lv_label_set_text(s_toast, text);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    s_toast_until = esp_timer_get_time() + ms * 1000LL;
}

// ---------------------------------------------------------------- browsing

static bool at_settings(void)
{
    return !strcmp(s_stack[s_depth - 1].id, SETTINGS_ID);
}

static bool at_version(void)
{
    return !strcmp(s_stack[s_depth - 1].id, VERSION_ID);
}

// Settings or Version: pages the board makes itself, not the bridge's folders.
static bool at_local(void)
{
    return at_settings() || at_version();
}

// The Settings row shown under the note when there's no bridge (note_settings_row).
static const cd_entry_t s_settings_entry = {.id = SETTINGS_ID, .title = "Settings", .container = true, .children = -1};

static bool settings_under_note(void)
{
    return s_note && !lv_obj_has_flag(s_note, LV_OBJ_FLAG_HIDDEN) && !lv_obj_has_flag(s_row[ROWS - 1], LV_OBJ_FLAG_HIDDEN);
}

enum { SET_TOUCH, SET_PICTURE, SET_ROTATION, SET_BRIGHT, SET_DIM, SET_OFF, SET_OTA, SET_WEB, SET_FIND, SET_VERSION, SET_N };

// The first item on a page of a folder of total items: the last page shows the last full
// page's worth instead of a few rows (it overlaps the page before).
static int page_first(int page, int total)
{
    int first = page * ROWS;
    return first + ROWS > total && total > ROWS ? total - ROWS : first;
}

// Settings, a page at a time like any folder.
static void settings_rows(void)
{
    static const char *const titles[SET_N] = {"Touch test", "Picture", "Rotation", "Backlight", "Dim", "Off",
                                              "OTA updates", "Web control", "Find the bridge again", "Version"};
    static const char *const web[] = {"full", "read-only", "off"};
    static const char *const rotations[] = {"auto", "0\u00b0", "90\u00b0", "180\u00b0", "270\u00b0"};
    int first = page_first(s_stack[s_depth - 1].page, SET_N);
    s_count = 0;
    for (int k = first; k < SET_N && s_count < ROWS; k++) {
        cd_entry_t *e = &s_entries[s_count++];
        memset(e, 0, sizeof(*e));
        snprintf(e->id, sizeof(e->id), "#set%d", k);
        if (k == SET_PICTURE) {
            snprintf(e->title, sizeof(e->title), "Picture: %s", !strcmp(bridge_picture(), "4x3") ? "4:3 centre" : "16:9");
        } else if (k == SET_WEB) {
            snprintf(e->title, sizeof(e->title), "Web control: %s", web[app_web_mode()]);
        } else if (k == SET_OTA) {
            snprintf(e->title, sizeof(e->title), "OTA updates: %s", app_ota_enabled() ? "on" : "off");
        } else if (k == SET_VERSION) {
            snprintf(e->title, sizeof(e->title), "Version & board: %s", CHANNELSTV_VERSION);
        } else if (k == SET_ROTATION) {
            snprintf(e->title, sizeof(e->title), "Rotation: %s", rotations[rotate_setting() + 1]);
        } else if (k == SET_BRIGHT) {
            snprintf(e->title, sizeof(e->title), "Backlight: %d%%", s_bright);
        } else if (k == SET_DIM) { // on battery
            if (!s_dim_s) {
                strlcpy(e->title, "Dim: never", sizeof(e->title));
            } else if (s_dim_s < 60) {
                snprintf(e->title, sizeof(e->title), "Dim: %d s", s_dim_s);
            } else {
                snprintf(e->title, sizeof(e->title), "Dim: %d min", s_dim_s / 60);
            }
        } else if (k == SET_OFF) {
            if (s_off_min) {
                snprintf(e->title, sizeof(e->title), "Off: %d min", s_off_min); // on battery
            } else {
                strlcpy(e->title, "Off: never", sizeof(e->title));
            }
        } else {
            strlcpy(e->title, titles[k], sizeof(e->title));
        }
    }
    s_total = SET_N;
    s_stack[s_depth - 1].total = s_total;
}

// What Version opens, under the heading "Version x.y.z": the board's address, and the bridge's
// address, version, how it was found and its whole name: rows that only show, each a name over
// its value. A name too long for the row scrolls once when the page opens.
static bool s_version_opened;
static void version_rows(void)
{
    net_t net;
    app_net(&net);
    char base[96], ver[24];
    bridge_base(base, sizeof(base));
    bridge_version(ver, sizeof(ver));
    // "http://host:port" to "host:port", and no port when it is the usual one
    const char *addr = strstr(base, "://") ? strstr(base, "://") + 3 : base;
    char host[64];
    strlcpy(host, addr, sizeof(host));
    char *p = strrchr(host, ':');
    if (p && !strcmp(p, ":8200")) {
        *p = '\0';
    }
    // every row first ("name\nvalue"), then the page's share of them
    static EXT_RAM_BSS_ATTR char rows[40][sizeof(s_entries[0].title)];
    int n = 0;
#define ROW(...) snprintf(rows[n++], sizeof(rows[0]), __VA_ARGS__)
    ROW("Board IP\n%s", net.ip[0] ? net.ip : "none");
    if (base[0]) {
        ROW("Bridge (%s)\n%s", bridge_found_by(), host);
        ROW("Bridge version\n%s", ver[0] ? ver : "unknown");
        ROW("Bridge name\n%s", bridge_name()); // in full: the row with id #version4
    } else {
        ROW("Bridge\nnone");
    }
    // the board and its chips, as on /chips (no network name or MAC: screenshots stay shareable)
    const board_info_t *bi = board_info();
    const char *short_name = strstr(bi->name, "Touch-") ? strstr(bi->name, "Touch-") + 6 : bi->name;
    ROW("Board\n%s%s%s", short_name, bi->rev[0] ? " " : "", bi->rev); // "AMOLED-1.8 V2"
    ROW("Display\n%s", bi->display);
    char *json = heap_caps_malloc(3072, MALLOC_CAP_SPIRAM);
    if (json) {
        board_chips_json(json, 3072);
        cJSON *chips = cJSON_Parse(json);
        const cJSON *c;
        cJSON_ArrayForEach(c, chips) {
            const char *part = cJSON_GetStringValue(cJSON_GetObjectItem(c, "part"));
            const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(c, "id"));
            if (!part || !strcmp(part, "Wi-Fi") || n >= 34) {
                continue; // its "ID" is the MAC
            }
            ROW("%s %s\n%s", part, cJSON_IsTrue(cJSON_GetObjectItem(c, "ok")) ? "ok" : "missing", id ? id : "");
        }
        cJSON_Delete(chips);
        heap_caps_free(json);
    }
    char sha[9];
    esp_app_get_elf_sha256(sha, sizeof(sha)); // the build, where there's no build date to show
    ROW("Firmware\n%s (%s)", CHANNELSTV_VERSION, sha);
    wifi_ap_record_t ap;
    if (net.connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        ROW("Wi-Fi\n%d dBm, channel %d", net.rssi, ap.primary);
    } else {
        ROW("Wi-Fi\nnot connected");
    }
    battery_t bat;
    board_battery(&bat);
    if (bat.mv > 0) {
        ROW("Battery\n%.2f V, %d %%%s", bat.mv / 1000.0, bat.percent, bat.charging ? ", charging" : bat.usb ? ", on USB" : "");
    } else {
        ROW("Battery\nnone%s", bat.usb ? ", on USB" : "");
    }
    ROW("Internal RAM free\n%u KB, lowest %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
        (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
    ROW("PSRAM free\n%u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    int64_t up = esp_timer_get_time() / 1000000;
    ROW("Up\n%dh %02dm", (int)(up / 3600), (int)(up / 60 % 60));
    ROW("Last start\n%s", app_reset_reason());
#undef ROW
    s_count = 0;
    for (int k = page_first(s_stack[s_depth - 1].page, n); k < n && s_count < ROWS; k++) {
        cd_entry_t *e = &s_entries[s_count++];
        memset(e, 0, sizeof(*e));
        // the bridge's whole name keeps its id: it scrolls once when the page opens
        snprintf(e->id, sizeof(e->id), "#version%d", strncmp(rows[k], "Bridge name\n", 12) ? 100 + k : 4);
        e->play_count = -1;
        strlcpy(e->title, rows[k], sizeof(e->title));
    }
    s_total = n;
    s_stack[s_depth - 1].total = s_total;
}

// The top folder lists the bridge's folders and, after them, Settings.
static void browse_task(void *arg)
{
    static EXT_RAM_BSS_ATTR cd_entry_t fresh[ROWS_MAX];
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
            if (n >= 0) {
                s_stack[s_depth - 1].total = total;
            }
            s_loading_us = 0;
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
    if (at_local()) {
        if (at_settings()) {
            settings_rows();
        } else {
            version_rows();
        }
        s_result_gen++;
        return;
    }
    request_t req;
    strlcpy(req.id, lv->id, sizeof(req.id));
    // the folder's own count: s_total may still be that of the folder just left
    req.start = lv->page ? page_first(lv->page, lv->total) : 0;
    if (s_count > 0) {
        s_loading_us = esp_timer_get_time(); // the rows stay until the new ones come
    } else {
        s_count = -2;
        s_result_gen++;
    }
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
    lv->total = 0;
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

// A fling: a quick swipe slides through many pages, slowing down, with the page number large
// over the list; only the page it stops on is fetched. A tap stops it where it is.
static lv_obj_t *s_fling_box, *s_fling_lbl;
static lv_timer_t *s_fling_timer;
static int s_fling_from, s_fling_to, s_fling_at, s_fling_pages;
static int64_t s_fling_t0, s_fling_us;

// Pages a vertical swipe of dy px in ms moves: one for a slow one; past 2 screens a second, the
// square of the speed over two (5 screens a second: 12 pages)
static int ui_fling_pages(int dy, int ms, int screen_h)
{
    if (ms <= 0 || screen_h <= 0) {
        return 1;
    }
    float sps = (float)abs(dy) / screen_h * 1000.0f / ms;
    if (sps < 2.0f) {
        return 1;
    }
    int n = (int)(sps * sps / 2 + 0.5f);
    return n < 1 ? 1 : n;
}

static void fling_end(void)
{
    if (s_fling_timer) {
        lv_timer_delete(s_fling_timer);
        s_fling_timer = NULL;
    }
    show(s_fling_box, false);
    level_t *lv = &s_stack[s_depth - 1];
    if (lv->page != s_fling_at) {
        lv->page = s_fling_at;
        request_page();
    }
}

static void fling_step(lv_timer_t *t)
{
    float p = (esp_timer_get_time() - s_fling_t0) / (float)s_fling_us;
    p = p > 1 ? 1 : p;
    p = 1 - (1 - p) * (1 - p); // easing out: fast at first, slowing to a stop
    s_fling_at = s_fling_from + (int)((s_fling_to - s_fling_from) * p + (s_fling_to > s_fling_from ? 0.5f : -0.5f));
    lv_label_set_text_fmt(s_fling_lbl, "%d / %d", s_fling_at + 1, s_fling_pages);
    if (p >= 1) {
        fling_end();
    }
}

static void turn_page(int dir)
{
    level_t *lv = &s_stack[s_depth - 1];
    int pages = (s_total + ROWS - 1) / ROWS;
    if (pages <= 1 || s_count < 0 || s_loading_us) {
        return;
    }
    int n = s_swipe_ms ? ui_fling_pages(s_swipe_dy, s_swipe_ms, LCD_H) : 1;
    s_swipe_ms = 0;
    if (n <= 1) {
        int p = lv->page + dir;
        lv->page = p < 0 ? pages - 1 : p >= pages ? 0 : p;
        request_page();
        return;
    }
    // many pages: slide, and stop at the first or last page rather than going round
    int from = s_fling_timer ? s_fling_at : lv->page, to = from + dir * n;
    to = to < 0 ? 0 : to >= pages ? pages - 1 : to;
    if (to == from) {
        return;
    }
    s_fling_from = from;
    s_fling_to = to;
    s_fling_at = from;
    s_fling_pages = pages;
    s_fling_t0 = esp_timer_get_time();
    int d = abs(to - from);
    s_fling_us = (300 + 90 * (int)(log2f(d) * 2)) * 1000LL; // a longer slide for more pages, under 2 s
    show(s_fling_box, true);
    if (!s_fling_timer) {
        s_fling_timer = lv_timer_create(fling_step, 30, NULL);
    }
    fling_step(NULL);
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
// what plays, for seeking and resuming: the stream without ?t=, the item (recordings only),
// where the stream started and how long the recording is (0: unknown)
static char s_play_base[256], s_play_id[80];
static int s_play_at_s, s_play_dur_s;
static bool s_seekable;
static volatile int s_seek_s;         // the seek tapped so far, applied at s_seek_due
static volatile int64_t s_seek_due;
// the playback controls: a 3x3 grid drawn over the picture, the item's details, the seek under way
static uint16_t *s_ov_grid, *s_ov_info, *s_ov_seek;
static uint16_t *s_ov_old[2]; // replaced overlays, freed two replacements later: the drawer is done
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
    lv_obj_set_size(spin, PX(64), PX(64));
    lv_obj_align(spin, LV_ALIGN_CENTER, 0, -PX(40));
    lv_obj_remove_flag(spin, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(spin, PX(6), LV_PART_MAIN);
    lv_obj_set_style_arc_width(spin, PX(6), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spin, lv_color_hex(C_PANEL), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spin, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_t *name = label(s_tune, f18, C_TEXT);
    lv_obj_set_size(name, LCD_W - PX(20), PX(24));
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_label_set_text(name, title);
    lv_obj_align(name, LV_ALIGN_CENTER, 0, PX(18));
    s_tune_secs = label(s_tune, f18, C_ACCENT);
    lv_label_set_text(s_tune_secs, "Tuning...");
    lv_obj_align(s_tune_secs, LV_ALIGN_CENTER, 0, PX(46));
    lv_obj_t *hint = label(s_tune, f14, C_DIM);
    lv_label_set_text(hint, "Tap to cancel");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -PX(14));
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
static void fmt_time(char *out, size_t len, int s)
{
    if (s >= 3600) {
        snprintf(out, len, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    } else {
        snprintf(out, len, "%d:%02d", s / 60, s % 60);
    }
}

// Where a recording is: where its stream started plus how far into it the picture is.
static int play_pos_s(void)
{
    return s_play_at_s + tv_position_ms() / 1000;
}

// "12:40 / 44:00" (after pre) and a bar, or "12:40" when the length isn't known
static void position_line(lv_obj_t *parent, const char *pre, int at_s, int y)
{
    char a[16], d[16], text[48];
    fmt_time(a, sizeof(a), at_s);
    fmt_time(d, sizeof(d), s_play_dur_s);
    if (s_play_dur_s) {
        snprintf(text, sizeof(text), "%s%s / %s", pre, a, d);
    } else {
        snprintf(text, sizeof(text), "%s%s", pre, a);
    }
    lv_obj_t *l = label(parent, f22, C_TEXT);
    lv_label_set_text(l, text);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, PX(y));
    if (s_play_dur_s) {
        lv_obj_t *bar = lv_bar_create(parent);
        lv_obj_set_size(bar, LCD_W - PX(40), PX(8));
        lv_bar_set_range(bar, 0, s_play_dur_s);
        lv_bar_set_value(bar, at_s < s_play_dur_s ? at_s : s_play_dur_s, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x3a434d), LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
        lv_obj_align(bar, LV_ALIGN_CENTER, 0, PX(y + 24));
    }
}

static lv_obj_t *black_screen(void)
{
    lv_obj_t *o = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, LCD_W, LCD_H);
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

// The details: the title, live or a recording, where it is in its folder and, for a recording,
// how far it has got. Call with the LVGL lock held.
static uint16_t *make_info(void)
{
    lv_obj_t *in = black_screen();
    lv_obj_t *t = label(in, f22, C_ACCENT);
    lv_obj_set_width(t, LCD_W - PX(20));
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(t, s_play_title);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, PX(s_play_live ? -30 : -50));
    lv_obj_t *d = label(in, f14, C_DIM);
    char text[96];
    if (s_now_index >= 0) {
        snprintf(text, sizeof(text), "%s, %d of %d\n\nTap to go back to the picture", s_play_live ? "Live" : "Recording",
                 s_now_index + 1, s_now_total);
    } else {
        snprintf(text, sizeof(text), "%s\n\nTap to go back to the picture", s_play_live ? "Live" : "Recording");
    }
    lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(d, text);
    lv_obj_align(d, LV_ALIGN_CENTER, 0, PX(s_play_live ? 50 : 70));
    if (!s_play_live) {
        position_line(in, "", tv_playing() ? play_pos_s() : s_play_at_s, 10);
    }
    return render(in, LCD_W, LCD_H);
}

// What a seek tapped so far comes to: "+1:30" over "12:40 / 44:00".
static int seek_target(void)
{
    int to = play_pos_s() + s_seek_s;
    if (s_play_dur_s && to > s_play_dur_s - 5) {
        to = s_play_dur_s - 5;
    }
    return to < 0 ? 0 : to;
}

static uint16_t *make_seek(void)
{
    lv_obj_t *o = black_screen();
    char by[16], text[24];
    fmt_time(by, sizeof(by), abs(s_seek_s));
    snprintf(text, sizeof(text), "%s%s", s_seek_s < 0 ? "-" : "+", by);
    lv_obj_t *l = label(o, f28, C_ACCENT);
    lv_label_set_text(l, text);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, -PX(40));
    position_line(o, "→ ", seek_target(), 10);
    return render(o, LCD_W, LCD_H);
}

static void retire(uint16_t *img)
{
    heap_caps_free(s_ov_old[1]);
    s_ov_old[1] = s_ov_old[0];
    s_ov_old[0] = img;
}

// In the decoder (tv_call_locked), which holds the LVGL lock while the picture plays.
static void info_locked(void *arg)
{
    uint16_t *img = make_info();
    if (img) {
        retire(s_ov_info);
        s_ov_info = img;
    }
}

static void seek_locked(void *arg)
{
    uint16_t *img = make_seek();
    if (img) {
        retire(s_ov_seek);
        s_ov_seek = img;
    }
}

static void make_overlays(const char *title, bool live)
{
    static const char *const cells[9] = {
        LV_SYMBOL_DOWN "\nCH -", LV_SYMBOL_LIST "\nInfo", LV_SYMBOL_UP "\nCH +",
        LV_SYMBOL_MINUS "\nVOL", LV_SYMBOL_STOP "\nStop", LV_SYMBOL_PLUS "\nVOL",
        LV_SYMBOL_LEFT "\nBack", "", LV_SYMBOL_CLOSE "\nHide"};
    heap_caps_free(s_ov_grid);
    heap_caps_free(s_ov_info);
    heap_caps_free(s_ov_seek);
    s_ov_seek = NULL;
    lv_obj_t *g = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(g);
    lv_obj_set_size(g, LCD_W, LCD_H);
    lv_obj_set_style_bg_color(g, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
    for (int i = 0; i < 9; i++) {
        bool ch = (i == 0 || i == 2) && live;
        bool on = i != 7 && (ch || (i != 0 && i != 2) || s_now_index >= 0);
        int cw = LCD_W / 3, chh = LCD_H / 3; // the whole screen: square on the 1.54
        lv_obj_t *c = box(g, (i % 3) * cw + 2, (i / 3) * chh + 2, cw - 4, chh - 4,
                          i == 4 ? 0x5a1f1f : i == 7 ? 0x000000 : C_PANEL);
        lv_obj_t *l = label(c, i == 7 ? f14 : f18, on ? C_TEXT : C_DIM);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        if (i == 7) {
            lv_obj_set_width(l, LCD_W / 3 - 4);
            lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
            lv_label_set_text(l, title);
        } else if (s_seekable && (i == 3 || i == 5)) {
            lv_label_set_text(l, i == 3 ? LV_SYMBOL_PREV "\n30 s" : LV_SYMBOL_NEXT "\n30 s"); // volume: the keys
        } else {
            lv_label_set_text(l, cells[i]);
        }
        lv_obj_center(l);
    }
    s_ov_grid = render(g, LCD_W, LCD_H);
    s_ov_info = make_info();
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
        char text[200], why[96];
        snprintf(text, sizeof(text), "%s\n%s", tv_busy() ? "The bridge is busy" : "Can't play this",
                 tv_busy() ? "it runs all the conversions it allows" : tv_error());
        bool undecodable = !strcmp(tv_error(), TV_UNDECODABLE) || strstr(tv_error(), "doesn't fit");
        if (undecodable && bridge_recheck_profiles() && bridge_profile_problem(why, sizeof(why))) {
            snprintf(text, sizeof(text), "%s\n(%s)", BRIDGE_PROFILE_HELP, why);
        }
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

// Says so (and plays nothing) when the bridge's client profile can't give a stream the board
// decodes. Call with the LVGL lock held.
static bool profile_toast(void)
{
    char why[96], text[200];
    if (!bridge_profile_problem(why, sizeof(why))) {
        return false;
    }
    snprintf(text, sizeof(text), "%s\n(%s)", BRIDGE_PROFILE_HELP, why);
    toast(text, 6000);
    return true;
}

// How a recording's stream is asked to start at_s seconds in: the one place that knows the
// bridge's way of seeking. A DLNA time seek: stream.c turns the #npt= ending into a
// TimeSeekRange.dlna.org: npt=<seconds>- request header.
static void seek_request(char *url, size_t len, const char *base, int at_s)
{
    strlcpy(url, base, len);
    if (at_s > 0) {
        char t[24];
        snprintf(t, sizeof(t), "#npt=%d-", at_s);
        strlcat(url, t, len);
    }
}

static bool s_offer_seek; // the item about to start is offered with time seek (play_entry)

// at_s: where a recording starts, for a bridge that seeks
static bool start_play(const char *url, const char *title, bool live, int at_s)
{
    if (s_play_busy) {
        return false;
    }
    char why[96];
    if (bridge_profile_problem(why, sizeof(why))) {
        if (lvgl_port_lock(100)) { // recursive: fine from a tap, which holds it already
            profile_toast();
            lvgl_port_unlock();
        }
        return false;
    }
    s_play_busy = true;
    char base[256], name[96];
    strlcpy(base, url, sizeof(base)); // url may be s_play_base itself (a seek)
    strlcpy(name, title, sizeof(name));
    strlcpy(s_play_base, base, sizeof(s_play_base));
    strlcpy(s_play_title, name, sizeof(s_play_title));
    s_play_live = live;
    s_seekable = !live && s_offer_seek;
    s_play_at_s = s_seekable && at_s > 0 ? at_s : 0;
    s_seek_s = 0;
    seek_request(s_play_url, sizeof(s_play_url), base, s_play_at_s);
    if (xTaskCreate(play_task, "play", 6144, NULL, 4, NULL) != pdPASS) {
        s_play_busy = false;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- resuming recordings

// Where each recording was left, in NVS namespace "resume", keyed by a hash of its id. If NVS
// fills up, the namespace starts over: these are the first thing to give up.
static void resume_key(const char *id, char *key)
{
    uint32_t h = 2166136261u; // FNV-1a
    for (const char *c = id; *c; c++) {
        h = (h ^ (uint8_t)*c) * 16777619u;
    }
    sprintf(key, "p%08lx", (unsigned long)h);
}

static int resume_load(const char *id, int dur_s)
{
    char key[16];
    int32_t at = 0;
    nvs_handle_t h;
    resume_key(id, key);
    if (nvs_open("resume", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, key, &at);
        nvs_close(h);
    }
    return at >= RESUME_MIN_S && (!dur_s || at < dur_s - RESUME_END_S) ? at : 0;
}

// at_s 0 (or near either end): forget it. Call from a task with its stack in internal RAM.
static void resume_save(const char *id, int at_s, int dur_s)
{
    char key[16];
    nvs_handle_t h;
    resume_key(id, key);
    if (nvs_open("resume", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (at_s < RESUME_MIN_S || (dur_s && at_s >= dur_s - RESUME_END_S)) {
        nvs_erase_key(h, key);
    } else if (nvs_set_i32(h, key, at_s) == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        nvs_erase_all(h);
        nvs_set_i32(h, key, at_s);
    }
    nvs_commit(h);
    nvs_close(h);
}

// Where a recording starts: where the bridge says it was left (upnp:lastPlaybackPosition, shared
// with the DVR's other players), else where this board left it; 0 within its first 30 s or last
// minute.
static int resume_at(const char *id, int bridge_pos_s, int dur_s)
{
    if (bridge_pos_s < 0) {
        return resume_load(id, dur_s);
    }
    return bridge_pos_s >= RESUME_MIN_S && (!dur_s || bridge_pos_s < dur_s - RESUME_END_S) ? bridge_pos_s : 0;
}

static void play_entry(const char *folder, int index, int total, const cd_entry_t *e)
{
    strlcpy(s_now_folder, folder, sizeof(s_now_folder));
    s_now_index = index;
    s_now_total = total;
    s_play_dur_s = e->live ? 0 : e->duration_s;
    strlcpy(s_play_id, e->live ? "" : e->id, sizeof(s_play_id));
    s_offer_seek = !e->live && e->time_seek;
    start_play(e->url, e->title, e->live, s_offer_seek ? resume_at(e->id, e->last_pos_s, e->duration_s) : 0);
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

// A tap on a seek key: adds up, shows where it lands, and goes there once the taps stop.
static void seek_tap(int by_s)
{
    s_seek_s += by_s;
    s_seek_due = esp_timer_get_time() + SEEK_IDLE_MS * 1000LL;
    if (tv_call_locked(seek_locked, NULL) && s_ov_seek) {
        overlay(s_ov_seek);
    }
}

// The seek tapped (or asked for on the web page) is due: the stream starts again from there.
static void grid_locked(void *arg)
{
    make_overlays(s_play_title, s_play_live);
}

// Asks the bridge first whether it still offers the recording with time seek (its client
// settings may have changed since the folder was read): one that doesn't would refuse the
// seek. Then the controls go back to volume keys instead, and the recording plays on.
static void seek_now(void)
{
    int to = seek_target(), total = 0;
    s_seek_s = 0;
    overlay(NULL);
    static EXT_RAM_BSS_ATTR cd_entry_t e; // off ctl_task's stack: browsing needs its room
    // from the web page there's no place in a folder to read it again: the answer to the seek
    // tells (a stream that comes from the top stops the seeking)
    bool offered = s_now_index < 0 || (bridge_browse(s_now_folder, s_now_index, 1, &e, &total) == 1 &&
                                       !strcmp(e.id, s_play_id) && e.time_seek);
    if (!offered) {
        s_seekable = false;
        tv_call_locked(grid_locked, NULL);
        return;
    }
    s_offer_seek = true;
    start_play(s_play_base, s_play_title, false, to);
}

// While a recording plays, where it is goes to NVS now and then, and when it ends: at its real
// end it's forgotten, otherwise the next start resumes there.
static void resume_tick(void)
{
    static char id[80];
    static int at_s = -1, dur_s;
    static int64_t next;
    int64_t now = esp_timer_get_time();
    if (tv_playing() && tv_first_picture_ms() && s_play_id[0] && s_seekable) {
        if (strcmp(id, s_play_id)) {
            strlcpy(id, s_play_id, sizeof(id));
            next = now + RESUME_EVERY_US;
        }
        at_s = play_pos_s();
        dur_s = s_play_dur_s;
        if (now > next) {
            resume_save(id, at_s, dur_s);
            next = now + RESUME_EVERY_US;
        }
    } else if (at_s >= 0 && !tv_playing()) {
        // an end before the last minute (the bridge's stream can end early) keeps the place, so
        // play starts there again; resume_save forgets it within the last minute
        resume_save(id, strcmp(tv_end_reason(), "end") || dur_s ? at_s : 0, dur_s);
        at_s = -1;
        id[0] = '\0';
    }
}

static void ctl_task(void *arg)
{
    ctl_t c;
    while (true) {
        if (!xQueueReceive(s_ctl, &c, pdMS_TO_TICKS(100))) {
            if (s_play_at_s && tv_first_picture_ms() && tv_seek_ignored()) {
                // a proxy on the way dropped the seek: it plays from the top, and can't seek
                s_play_at_s = 0;
                s_seekable = false;
                tv_call_locked(grid_locked, NULL);
            }
            resume_tick();
            if (s_seek_s && esp_timer_get_time() > s_seek_due) {
                if (tv_playing() && s_seekable && !s_play_busy) {
                    seek_now();
                } else {
                    s_seek_s = 0;
                }
            }
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
        if ((s_ov_shown != s_ov_grid && (s_ov_shown != s_ov_seek || !s_ov_seek)) || !s_ov_grid) {
            overlay(s_ov_shown || !s_ov_grid ? NULL : s_ov_grid); // a tap on the picture or the details
            continue;
        }
        s_ov_until = esp_timer_get_time() + OVERLAY_MS * 1000LL;
        int row = c.y * 3 / LCD_H, col = c.x * 3 / LCD_W;
        int cell = (row > 2 ? 2 : row) * 3 + (col > 2 ? 2 : col);
        switch (cell) {
        case 0: step_item(-1); break;
        case 1:
            if (!s_play_live) {
                tv_call_locked(info_locked, NULL); // where it is now
            }
            overlay(s_ov_info);
            break;
        case 2: step_item(1); break;
        case 3: s_seekable ? seek_tap(-SEEK_STEP_S) : app_set_volume(app_volume() - 5); break;
        case 4: overlay(NULL); tv_stop(); break;
        case 5: s_seekable ? seek_tap(SEEK_STEP_S) : app_set_volume(app_volume() + 5); break;
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

static void display_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        int32_t v;
        if (nvs_get_i32(h, "bright", &v) == ESP_OK && v >= 20 && v <= 100) {
            s_bright = v;
        }
        if (nvs_get_i32(h, "dim", &v) == ESP_OK && v >= 0 && v <= 600) {
            s_dim_s = v > 0 && v <= 10 ? v * 60 : v; // saved in minutes before the 30 s choice
        }
        if (nvs_get_i32(h, "off", &v) == ESP_OK && v >= 0 && v <= 30) {
            s_off_min = v;
        }
        nvs_close(h);
    }
}

static void display_save(const char *key, int v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, key, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

int ui_brightness(void)
{
    return s_bright;
}

void ui_set_brightness(int percent)
{
    percent = percent < 20 ? 20 : percent > 100 ? 100 : percent;
    s_bright = percent;
    display_save("bright", percent);
    if (!s_dimmed && !s_screen_off) {
        board_backlight(percent);
    }
}

int ui_dim_seconds(void)
{
    return s_dim_s;
}

void ui_set_dim_seconds(int seconds)
{
    s_dim_s = seconds <= 0 ? 0 : seconds < 60 ? 30 : seconds < 300 ? 60 : seconds < 600 ? 300 : 600;
    display_save("dim", s_dim_s);
}

int ui_off_minutes(void)
{
    return s_off_min;
}

void ui_set_off_minutes(int minutes)
{
    s_off_min = minutes <= 0 ? 0 : minutes < 5 ? 1 : minutes < 10 ? 5 : minutes < 30 ? 10 : 30;
    display_save("off", s_off_min);
}

// Each board keeps its own: the 1.54's under the name it always had
static const char *cal_key(void)
{
    return board_info()->id == BOARD_AMOLED18 ? "touchcal_a18" : "touchcal";
}

static void cal_load(void)
{
    nvs_handle_t h;
    touch_cal_t c;
    size_t len = sizeof(c);
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, cal_key(), &c, &len) == ESP_OK && len == sizeof(c)) {
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
        int cx, cy;
        cal_at(i, &cx, &cy);
        tx[i] = cx;
        ty[i] = cy;
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
            nvs_set_blob(h, cal_key(), &c[best], sizeof(c[best]));
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
    if (s_loading_us) {
        return; // the rows on the screen are about to go
    }
    if (i < 0 || i >= s_count) {
        return;
    }
    const cd_entry_t *e = &s_entries[i];
    if (at_version()) {
        return; // rows that only show
    }
    if (at_settings()) {
        switch (page_first(s_stack[s_depth - 1].page, SET_N) + i) {
        case SET_TOUCH:
            s_view = VIEW_CAL;
            s_cal_step = 0;
            break;
        case SET_PICTURE:
            bridge_set_picture(!strcmp(bridge_picture(), "4x3") ? "16x9" : "4x3");
            settings_rows();
            break;
        case SET_ROTATION: // auto, 0, 90, 180, 270, auto, ...
            rotate_set(rotate_setting() >= 3 ? ROTATE_AUTO : rotate_setting() + 1);
            settings_rows();
            break;
        case SET_BRIGHT: // 20, 40, 60, 80, 100, 20, ...
            ui_set_brightness(s_bright >= 100 ? 20 : (s_bright / 20 + 1) * 20);
            settings_rows();
            break;
        case SET_DIM: // 30 s, 1, 5, 10 min, never, 30 s, ...
            ui_set_dim_seconds(s_dim_s == 30 ? 60 : s_dim_s == 60 ? 300 : s_dim_s == 300 ? 600 : s_dim_s == 600 ? 0 : 30);
            settings_rows();
            break;
        case SET_OFF: // 1, 5, 10, 30 min, never, 1, ...
            ui_set_off_minutes(s_off_min == 1 ? 5 : s_off_min == 5 ? 10 : s_off_min == 10 ? 30 : s_off_min == 30 ? 0 : 1);
            settings_rows();
            break;
        case SET_OTA:
            if (s_api_touch && !app_ota_enabled()) {
                toast("Only a finger on the board turns this on", 3000); // not /api/touch
                break;
            }
            app_set_ota_enabled(!app_ota_enabled());
            settings_rows();
            break;
        case SET_WEB: // read-only, off, full, read-only, ...
            if (s_api_touch) {
                toast("Only a finger on the board changes this", 3000); // not /api/touch
                break;
            }
            app_set_web_mode(app_web_mode() == WEB_READ_ONLY ? WEB_OFF : app_web_mode() == WEB_OFF ? WEB_FULL : WEB_READ_ONLY);
            settings_rows();
            break;
        case SET_FIND:
            bridge_rescan();
            toast("Searching...", 1500);
            break;
        case SET_VERSION: {
            cd_entry_t page = {.id = VERSION_ID, .container = true};
            snprintf(page.title, sizeof(page.title), "Version %s", CHANNELSTV_VERSION);
            s_version_opened = true;
            open_folder(&page);
            return;
        }
        }
        s_result_gen++;
        return;
    }
    if (e->container) {
        open_folder(e);
    } else if (!s_play_busy) {
        play_entry(s_stack[s_depth - 1].id, page_first(s_stack[s_depth - 1].page, s_total) + i, s_total, e);
    }
}

static void on_gesture(gesture_t g, int x, int y)
{
    if (s_fling_timer && g != G_UP && g != G_DOWN) {
        fling_end(); // a tap (or anything) stops the slide where it is
        return;
    }
    lv_display_trigger_activity(NULL);
    if (setup_active()) {
        return; // nothing to browse until the board is on a network
    }
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
            int r = (y - HEAD_H) / PAD_H, c = x * 3 / LCD_W;
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
            if ((y - HEAD_H) / ROW_H == ROWS - 1 && settings_under_note()) {
                open_folder(&s_settings_entry);
            } else {
                bridge_rescan(); // "Try again"
                request_page();
            }
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
    if (i >= 0 && i < ROWS && s_view == VIEW_LIST && (i < s_count || (i == ROWS - 1 && settings_under_note()))) {
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
            s_swipe_dy = dy;
            s_swipe_ms = ms;
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

// With no bridge to list, the top level still offers Settings, in its last row, under the note:
// Web control starts read-only, so the web page can't set the bridge's address until it's changed.
static void note_settings_row(void)
{
    int i = ROWS - 1;
    show(s_row_cap[i], false);
    show(s_row_new[i], false);
    lv_obj_set_style_text_font(s_row_text[i], f22, 0);
    lv_label_set_long_mode(s_row_text[i], LV_LABEL_LONG_DOT);
    lv_obj_set_height(s_row_text[i], PR(28));
    lv_obj_align(s_row_text[i], LV_ALIGN_LEFT_MID, PX(40), 0);
    lbl(s_row_text[i], s_settings_entry.title);
    lbl(s_row_icon[i], LV_SYMBOL_SETTINGS);
    lv_obj_set_style_bg_color(s_row[i], lv_color_hex(C_PANEL), 0);
    show(s_row[i], true);
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
        snprintf(text, sizeof(text), "No dlna-bridge found.\nInstall it, or set its\naddress on\nhttp://%s/\nwith Web control: full.\nTap to try again", net.ip);
        break;
    case BRIDGE_UNREACHABLE:
        snprintf(text, sizeof(text), "%s\nis not reachable.\nChange it on\nhttp://%s/\nwith Web control: full.\nTap to try again",
                 bridge_override(), net.ip);
        break;
    case BRIDGE_FOUND:
        snprintf(text, sizeof(text), "The bridge didn't answer.\n\nTap to try again");
        break;
    }
    note(text, C_WARN);
    if (s_depth == 1) {
        note_settings_row();
    }
}

static void rows_hidden(void)
{
    for (int i = 0; i < ROWS; i++) {
        show(s_row[i], false);
    }
}

// The setup hotspot: its QR code (a phone camera offers to join), what to do, how it's going.
// A label whose text doesn't fit scrolls it once, slowly, to the end; a moment later it
// settles back to the start in its usual mode (dots, or cut). One at a time.
#define SCROLL_PX_S 20
static lv_timer_t *s_scroll_timer;
static lv_obj_t *s_scroll_label;
static lv_label_long_mode_t s_scroll_settle;

static void scroll_settle(lv_timer_t *t)
{
    lv_timer_delete(t);
    s_scroll_timer = NULL;
    lv_label_set_long_mode(s_scroll_label, s_scroll_settle); // at the start again, dots placed
}

// text is the label's whole text: in dots mode LVGL has already shortened its own copy.
static void scroll_once(lv_obj_t *o, const char *text, lv_label_long_mode_t settle)
{
    if (s_scroll_timer) {
        scroll_settle(s_scroll_timer);
    }
    lv_obj_update_layout(o);
    lv_point_t size;
    lv_text_get_size(&size, text, lv_obj_get_style_text_font(o, 0),
                     lv_obj_get_style_text_letter_space(o, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int over = size.x - lv_obj_get_content_width(o);
    if (over <= 0) {
        return;
    }
    lv_obj_set_style_anim_duration(o, lv_anim_speed(SCROLL_PX_S), 0);
    lv_label_set_long_mode(o, LV_LABEL_LONG_MODE_SCROLL);
    s_scroll_label = o;
    s_scroll_settle = settle;
    s_scroll_timer = lv_timer_create(scroll_settle, over * 1000 / SCROLL_PX_S + 600, NULL);
}

// The heading's title as given: LVGL shortens it with dots in the label's own text, so that
// can't be compared or used to redo the dots at another width (head_fit).
static char s_head_text[112];

static void head_set(const char *text)
{
    if (strcmp(s_head_text, text) != 0) {
        strlcpy(s_head_text, text, sizeof(s_head_text));
        lv_label_set_text(s_head, s_head_text);
        if (s_depth == 1 && s_view == VIEW_LIST && !setup_active()) {
            scroll_once(s_head, s_head_text, LV_LABEL_LONG_MODE_DOTS); // the bridge's name, in full, once
        }
    }
}

static void setup_view(void)
{
    rows_hidden();
    show(s_pad, false);
    show(s_cal, false);
    show(s_head_back, false);
    show(s_qr, true);
    lbl(s_pageno, "");
    head_set("Wi-Fi setup");
    char status[96], text[160];
    setup_status(status, sizeof(status));
    snprintf(text, sizeof(text), "Join %s\n%s", SETUP_SSID, status[0] ? status : "then open any web page");
    note(text, setup_failed() ? C_WARN : C_TEXT);
    lv_obj_align(s_note, LV_ALIGN_TOP_MID, 0, HEAD_H + QR_PX + PX(18));
}

static void rebuild(void)
{
    level_t *lv = &s_stack[s_depth - 1];
    if (setup_active()) {
        setup_view();
        return;
    }
    show(s_qr, false);
    show(s_note, false);
    show(s_pad, s_view == VIEW_PAD);
    show(s_cal, s_view == VIEW_CAL);
    show(s_head_back, s_depth > 1 || s_view != VIEW_LIST);
    // the page number first, so the title is laid out once, at its final width
    char pageno[24] = "";
    int pages = s_view == VIEW_LIST && s_count > 0 ? (s_total + ROWS - 1) / ROWS : 0;
    if (pages > 1) {
        snprintf(pageno, sizeof(pageno), "%d/%d", lv->page + 1, pages);
    }
    lbl(s_pageno, pageno);
    head_fit();
    if (s_view == VIEW_CAL) {
        rows_hidden();
        head_set("Touch test");
        int cx, cy;
        cal_at(s_cal_step % 5, &cx, &cy);
        lv_obj_set_pos(s_cal, cx - PX(14), cy - PX(14));
        char text[96];
        snprintf(text, sizeof(text), "Tap the centre of the dot\n%d of 5", s_cal_step + 1);
        note(text, C_DIM);
        lv_obj_align(s_note, LV_ALIGN_CENTER, 0, s_cal_step == 4 ? PX(50) : 0);
        return;
    }
    lv_obj_align(s_note, LV_ALIGN_TOP_LEFT, PX(8), HEAD_H + PX(8));
    if (s_view == VIEW_PAD) {
        rows_hidden();
        char text[48];
        snprintf(text, sizeof(text), "Channel %s_", s_digits);
        head_set(text);
        return;
    }
    head_set(s_depth == 1 ? (bridge_name()[0] ? bridge_name() : "channelstv") : lv->title);
    if (bridge_state() != BRIDGE_FOUND && !at_local()) {
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
    if (s_scroll_timer && s_scroll_label != s_head && !at_version()) {
        lv_timer_delete(s_scroll_timer); // a row left mid-scroll: the next page sets its own mode
        s_scroll_timer = NULL;
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
        // the Version page's rows are "name\nvalue": the name small on top, the value under it;
        // the bridge's name is cut rather than dotted, and scrolls once when the page opens
        bool two = at_version(), name = two && !strcmp(e->id, "#version4");
        char *value = two ? strchr(text, '\n') : NULL;
        if (value) {
            *value++ = '\0';
            lbl(s_row_cap[i], text);
        }
        show(s_row_cap[i], value != NULL);
        bool scrolling = s_scroll_timer && s_scroll_label == s_row_text[i];
        lv_obj_set_style_text_font(s_row_text[i], value ? f18 : f22, 0);
        if (!scrolling) {
            lv_label_set_long_mode(s_row_text[i], name ? LV_LABEL_LONG_CLIP : LV_LABEL_LONG_DOT);
        }
        lv_obj_set_height(s_row_text[i], PR(value ? 22 : 28));
        lv_obj_align(s_row_text[i], LV_ALIGN_LEFT_MID, PX(40), PR(value ? 11 : 0));
        lbl(s_row_text[i], value ? value : text);
        if (name && value && s_version_opened) {
            s_version_opened = false;
            scroll_once(s_row_text[i], value, LV_LABEL_LONG_CLIP);
        }
        show(s_row_new[i], !at_local() && !e->container && !e->live && e->play_count == 0);
        lbl(s_row_icon[i], at_local() ? LV_SYMBOL_SETTINGS
                           : !strcmp(e->id, SETTINGS_ID) ? LV_SYMBOL_SETTINGS
                           : e->container ? LV_SYMBOL_DIRECTORY : e->live ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_VIDEO);
        lv_obj_set_style_bg_color(s_row[i], lv_color_hex(C_PANEL), 0);
        show(s_row[i], true);
    }
}

// The battery in the heading whenever the ADC gives a reading (the radio firmware's sign of a
// battery): the percentage and a symbol on battery, red at 15 % and under; the charge symbol
// and a symbol while charging. The heading's page number and title make room for it.
// The heading's title takes what the page number (when there is one), the battery and Wi-Fi
// leave: the number pad's typed channel needs it.
static int text_w(lv_obj_t *l)
{
    const char *t = lv_label_get_text(l);
    return lv_text_get_width(t, strlen(t), lv_obj_get_style_text_font(l, 0), 0);
}

// The heading's right end, from the right: Wi-Fi, the battery, the page number, each as wide as
// what it says now; the title takes the rest.
static void head_fit(void)
{
    int r = PX(6) + text_w(s_wifi) + PX(4), y = PX(9) + HEAD_DY;
    if (s_batt_shown) {
        int bw = text_w(s_batt) + 2;
        lv_obj_set_width(s_batt, bw);
        lv_obj_align(s_batt, LV_ALIGN_TOP_RIGHT, -r, y);
        r += bw + PX(6);
    }
    if (lv_label_get_text(s_pageno)[0]) {
        int pw = text_w(s_pageno) + 2;
        lv_obj_set_width(s_pageno, pw);
        lv_obj_align(s_pageno, LV_ALIGN_TOP_RIGHT, -r, y);
        r += pw + PX(6);
    }
    int w = LCD_W - PX(34) - r;
    if (lv_obj_get_style_width(s_head, 0) != w) {
        lv_obj_set_width(s_head, w);
        lv_obj_update_layout(s_head);           // the new width now, not at the next refresh,
        lv_label_set_text(s_head, s_head_text); // so the dots are placed for it
        if (s_scroll_timer && s_scroll_label == s_head) {
            scroll_once(s_head, s_head_text, LV_LABEL_LONG_MODE_DOTS); // measured for the new width
        }
    }
}

static const char *battery_symbol(int pct)
{
    return pct > 80 ? LV_SYMBOL_BATTERY_FULL : pct > 55 ? LV_SYMBOL_BATTERY_3
         : pct > 30 ? LV_SYMBOL_BATTERY_2 : pct > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
}

static void battery_line(void)
{
    bool shown = s_bat.mv > 0;
    if (shown != s_batt_shown) {
        s_batt_shown = shown;
        show(s_batt, shown);
        head_fit();
    }
    if (shown) {
        char text[24];
        if (s_rows > 4) {
            // the tall heading's title is in the rows' larger font: the symbol alone leaves it room
            snprintf(text, sizeof(text), "%s%s", s_bat.charging ? LV_SYMBOL_CHARGE : "", battery_symbol(s_bat.percent));
        } else if (s_bat.charging) {
            snprintf(text, sizeof(text), LV_SYMBOL_CHARGE "%d%%%s", s_bat.percent, battery_symbol(s_bat.percent));
        } else {
            snprintf(text, sizeof(text), "%d%%%s", s_bat.percent, battery_symbol(s_bat.percent));
        }
        if (strcmp(lv_label_get_text(s_batt), text)) {
            lbl(s_batt, text);
            head_fit(); // "9%" to "10%": the title's room changes
        }
        lcolor(s_batt, s_bat.percent <= 15 && !s_bat.charging ? C_BAD : C_DIM);
    }
}

static void refresh(lv_timer_t *timer)
{
    if (s_loading_us && esp_timer_get_time() - s_loading_us > LOADING_SHOW_US) {
        s_loading_us = 0;
        s_count = -2; // slow: say so
        s_result_gen++;
    }
    int64_t now = esp_timer_get_time();
    if (now - s_bat_at > 2000000 || !s_bat_at) {
        board_battery(&s_bat);
        s_bat_at = now;
    }
    // on battery, dim after s_dim_s and go dark after s_off_min; any touch or key brings it back.
    // They count from unplugging, not from the last touch while on USB.
    static bool was_on_battery;
    bool on_battery = !s_bat.usb;
    if (on_battery && !was_on_battery) {
        lv_display_trigger_activity(NULL);
    }
    was_on_battery = on_battery;
    int64_t inactive = lv_display_get_inactive_time(NULL);
    bool off = s_off_min && inactive > s_off_min * 60000LL;
    bool dim = off || (s_dim_s && inactive > s_dim_s * 1000LL);
    if (!on_battery) {
        // the timers don't run on USB, and plugging in doesn't wake the screen: a dim or dark
        // one stays so until a touch or key
        off = off && s_screen_off;
        dim = dim && s_dimmed;
    }
    if (dim != s_dimmed || off != s_screen_off) {
        s_dimmed = dim;
        s_screen_off = off;
        board_backlight(off ? 0 : dim ? BL_DIM : s_bright);
    }
    if (s_back_wanted) {
        s_back_wanted = false;
        go_back();
    }
    rotate_hold(s_view == VIEW_CAL); // the touch test needs the panel's own way up
    if (board_rotation_apply()) {
        lv_obj_invalidate(lv_screen_active());
        lv_obj_invalidate(lv_layer_top());
    }
    net_t net;
    app_net(&net);
    lcolor(s_wifi, !net.connected ? C_BAD : net.rssi > -60 ? C_GOOD : net.rssi > -72 ? C_WARN : C_BAD);
    battery_line();
    // the bridge found (again) or lost: start over from the top
    if (s_bridge_gen != bridge_gen()) {
        s_bridge_gen = bridge_gen();
        s_depth = 1;
        s_stack[0].page = 0;
        if (bridge_state() == BRIDGE_FOUND) {
            request_page();
            profile_toast();
        }
    }
    if (s_setup_gen != setup_gen()) {
        s_setup_gen = setup_gen();
        s_shown_gen = ~s_result_gen;
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
    lv_obj_set_size(s_power_arc, PX(120), PX(120));
    lv_obj_align(s_power_arc, LV_ALIGN_CENTER, 0, -PX(14));
    lv_arc_set_rotation(s_power_arc, 270);
    lv_arc_set_bg_angles(s_power_arc, 0, 360);
    lv_arc_set_range(s_power_arc, 0, 3000);
    lv_obj_remove_style(s_power_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_power_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_power_arc, PX(8), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_power_arc, PX(8), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_power_arc, lv_color_hex(0x2a323b), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_power_arc, lv_color_hex(C_TEXT), LV_PART_INDICATOR);
    lv_obj_t *icon = lv_label_create(s_power_arc);
    lv_obj_set_style_text_font(icon, sym(28), 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(C_TEXT), 0);
    lv_label_set_text(icon, LV_SYMBOL_POWER);
    lv_obj_center(icon);
    s_power_lbl = label(s_power, f18, C_TEXT);
    lv_obj_align(s_power_lbl, LV_ALIGN_BOTTOM_MID, 0, -PX(28));
    lv_obj_add_flag(s_power, LV_OBJ_FLAG_HIDDEN);
}

static void build_screen(lv_obj_t *scr)
{
    // the heading: back at the left end, the folder's name, the page, the battery, Wi-Fi
    s_head_back = label(scr, sym(20), C_ACCENT);
    lv_label_set_text(s_head_back, LV_SYMBOL_LEFT);
    lv_obj_align(s_head_back, LV_ALIGN_TOP_LEFT, PX(8), PX(6) + HEAD_DY);
    bool tall = s_rows > 4; // a heading as tall as a row: the rows' font for the title
    s_head = label(scr, tall ? f22 : f18, C_ACCENT);
    lv_obj_set_size(s_head, LCD_W - PX(34 + 70), tall ? PX(28) : PX(24));
    lv_label_set_long_mode(s_head, LV_LABEL_LONG_DOT);
    lv_obj_align(s_head, LV_ALIGN_TOP_LEFT, PX(34), tall ? (HEAD_H - PX(28)) / 2 : PX(7));
    s_pageno = label(scr, f14, C_DIM);
    lv_obj_set_width(s_pageno, PX(50));
    lv_label_set_long_mode(s_pageno, LV_LABEL_LONG_CLIP); // "12/67" on one line, never wrapped
    lv_obj_set_style_text_align(s_pageno, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_pageno, LV_ALIGN_TOP_RIGHT, -PX(26), PX(9) + HEAD_DY);
    s_wifi = label(scr, sym(14), C_DIM);
    lv_label_set_text(s_wifi, LV_SYMBOL_WIFI);
    lv_obj_align(s_wifi, LV_ALIGN_TOP_RIGHT, -PX(6), PX(9) + HEAD_DY);
    s_batt = label(scr, f14, C_TEXT); // "87%" and the battery's symbol (DejaVu, then Montserrat's)
    lv_obj_set_width(s_batt, PX(60)); // "100%" and the symbol
    lv_label_set_long_mode(s_batt, LV_LABEL_LONG_CLIP); // one line, never wrapped under the heading
    lv_obj_set_style_text_align(s_batt, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_batt, LV_ALIGN_TOP_RIGHT, -PX(24), PX(9) + HEAD_DY);
    lv_obj_add_flag(s_batt, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < ROWS_MAX; i++) {
        s_row[i] = box(scr, PX(4), HEAD_H + i * ROW_H + 1, LCD_W - PX(8), ROW_H - 2, C_PANEL);
        s_row_icon[i] = label(s_row[i], sym(20), C_ACCENT);
        lv_obj_align(s_row_icon[i], LV_ALIGN_LEFT_MID, PX(10), 0);
        s_row_new[i] = box(s_row[i], PX(31), (ROW_H - 2) / 2 - PX(19), PX(11), PX(11), C_WARN); // on the icon's top right corner
        lv_obj_set_style_radius(s_row_new[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(s_row_new[i], lv_color_hex(C_PANEL), 0);
        lv_obj_set_style_border_width(s_row_new[i], 2, 0);
        lv_obj_add_flag(s_row_new[i], LV_OBJ_FLAG_HIDDEN);
        s_row_text[i] = label(s_row[i], f22, C_TEXT);
        lv_obj_set_size(s_row_text[i], LCD_W - PX(8 + 46), PR(28)); // one line, cut short with "..."
        lv_label_set_long_mode(s_row_text[i], LV_LABEL_LONG_DOT);
        lv_obj_align(s_row_text[i], LV_ALIGN_LEFT_MID, PX(40), 0);
        s_row_cap[i] = label(s_row[i], f18, C_TEXT);
        lv_obj_set_size(s_row_cap[i], LCD_W - PX(8 + 46), PR(22));
        lv_label_set_long_mode(s_row_cap[i], LV_LABEL_LONG_DOT);
        lv_obj_align(s_row_cap[i], LV_ALIGN_LEFT_MID, PX(40), -PR(11));
        lv_obj_add_flag(s_row_cap[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_note = label(scr, f18, C_DIM);
    lv_obj_set_width(s_note, LCD_W - PX(16));
    lv_label_set_long_mode(s_note, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_note, LV_ALIGN_TOP_LEFT, PX(8), HEAD_H + PX(8));

    // the number pad: 3 x 4 keys under the heading
    s_pad = lv_obj_create(scr);
    lv_obj_remove_style_all(s_pad);
    lv_obj_set_pos(s_pad, 0, HEAD_H);
    lv_obj_set_size(s_pad, LCD_W, LCD_H - HEAD_H);
    lv_obj_remove_flag(s_pad, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 12; i++) {
        int kw = LCD_W / 3;
        lv_obj_t *k = box(s_pad, (i % 3) * kw + 3, (i / 3) * PAD_H + 2, kw - 6, PAD_H - 4, i == 11 ? 0x0277bd : C_PANEL);
        lv_obj_t *l = label(k, i == 11 ? sym(28) : f28, C_TEXT);
        char t[4] = {s_pad_keys[i], 0};
        lv_label_set_text(l, i == 11 ? LV_SYMBOL_PLAY : t);
        lv_obj_center(l);
    }
    lv_obj_add_flag(s_pad, LV_OBJ_FLAG_HIDDEN);

    // the touch test's dot
    s_cal = lv_obj_create(scr);
    lv_obj_remove_style_all(s_cal);
    lv_obj_set_size(s_cal, PX(28), PX(28));
    lv_obj_set_style_radius(s_cal, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_cal, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_cal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_cal, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_border_width(s_cal, 3, 0);
    lv_obj_remove_flag(s_cal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_cal, LV_OBJ_FLAG_HIDDEN);

    // the setup hotspot's QR code, dark on a white quiet zone
    s_qr = lv_qrcode_create(scr);
    lv_qrcode_set_size(s_qr, QR_PX);
    lv_qrcode_set_dark_color(s_qr, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(s_qr, lv_color_hex(0xffffff));
    lv_qrcode_update(s_qr, SETUP_QR, strlen(SETUP_QR));
    lv_obj_set_style_border_color(s_qr, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_width(s_qr, PX(6), 0);
    lv_obj_align(s_qr, LV_ALIGN_TOP_MID, 0, HEAD_H + PX(4));
    lv_obj_add_flag(s_qr, LV_OBJ_FLAG_HIDDEN);

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

// The panel keeps its own way up: LVGL draws the screen upright, and each block it hands over
// is turned in its own buffer, with its area, on the way to the panel.
static lv_display_flush_cb_t s_port_flush;

// The AMOLED panels take only windows that start on an even column and row and are an even
// number wide and high.
static void round_even(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

static void flush_turned(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    int rot = board_rotation();
    if (!rot) {
        s_port_flush(d, a, px);
        return;
    }
    int w = lv_area_get_width(a), h = lv_area_get_height(a), x, y;
    board_turn_in_place((uint16_t *)px, w, h, rot);
    board_turn_rect(a->x1, a->y1, w, h, rot, &x, &y);
    lv_area_t t = {x, y, x + (rot & 1 ? h : w) - 1, y + (rot & 1 ? w : h) - 1};
    s_port_flush(d, &t, px);
}

void ui_start(void)
{
    s_requests = xQueueCreate(4, sizeof(request_t));
    s_ctl = xQueueCreate(8, sizeof(ctl_t));
    strcpy(s_stack[0].id, "0");
    // as many rows as fit at about 64 px each (a comfortable finger), 4 to 6; with more than 4,
    // the heading takes a row's height too
    s_head_h = PX(32);
    s_rows = (LCD_H - s_head_h) / 64;
    s_rows = s_rows < 4 ? 4 : s_rows > ROWS_MAX ? ROWS_MAX : s_rows;
    if (s_rows > 4) {
        s_head_h = LCD_H / (s_rows + 1);
    }
    cal_load();
    display_load();
    lvgl_port_cfg_t port = ESP_LVGL_PORT_INIT_CONFIG();
    port.task_affinity = 0;
    port.task_stack = 8192;
    lvgl_port_init(&port);
    lvgl_port_display_cfg_t disp = {
        .io_handle = board_lcd_io(),
        .panel_handle = board_lcd_panel(),
        .buffer_size = LCD_W * DRAW_LINES, // internal DMA RAM, x2; playback borrows them too
        .double_buffer = true,
        .hres = LCD_W,
        .vres = LCD_H,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {.buff_dma = true, .swap_bytes = true},
    };
    lv_display_t *d = lvgl_port_add_disp(&disp);
    if (board_info()->id == BOARD_AMOLED18) {
        lv_display_add_event_cb(d, round_even, LV_EVENT_INVALIDATE_AREA, NULL);
    }
    s_port_flush = d->flush_cb;
    lv_display_set_flush_cb(d, flush_turned);
    if (board_touch()) {
        lvgl_port_touch_cfg_t touch = {.disp = d, .handle = board_touch()};
        lvgl_port_add_touch(&touch);
    }

    lvgl_port_lock(0);
    f14 = ttf(PX(14));
    f18 = ttf(PX(18));
    f22 = ttf(PX(22));
    f28 = ttf(PX(28));
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_text_color(scr, lv_color_hex(C_TEXT), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    build_screen(scr);

    s_toast = lv_label_create(lv_layer_top());
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_toast, PX(12), 0);
    lv_obj_set_style_pad_all(s_toast, PX(14), 0);
    lv_obj_set_style_text_font(s_toast, f22, 0);
    lv_obj_set_style_text_color(s_toast, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_align(s_toast, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_max_width(s_toast, LCD_W - PX(16), 0); // long errors wrap instead of running off
    lv_label_set_long_mode(s_toast, LV_LABEL_LONG_WRAP);
    lv_obj_center(s_toast);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    build_power_overlay();
    // the fling's page number, large over the list
    s_fling_box = box(lv_layer_top(), 0, 0, PX(150), PX(64), 0x000000);
    lv_obj_set_style_bg_opa(s_fling_box, LV_OPA_80, 0);
    lv_obj_center(s_fling_box);
    s_fling_lbl = label(s_fling_box, f28, C_TEXT);
    lv_obj_center(s_fling_lbl);
    show(s_fling_box, false);
    lv_timer_create(refresh, 100, NULL);
    refresh(NULL);
    lvgl_port_unlock();
    board_backlight(s_bright);
    tv_set_touch_handler(on_tv_touch);
    // stack in PSRAM: it only does network I/O
    xTaskCreateWithCaps(browse_task, "browse", 6144, NULL, 3, NULL, MALLOC_CAP_SPIRAM);
    // internal stack: the volume it sets is saved to NVS (flash), which a task with its stack
    // in PSRAM must not do
    xTaskCreate(ctl_task, "tvctl", 8192, NULL, 4, NULL); // a seek browses the bridge first
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
        s_api_touch = true;
        on_gesture(names[i].g, x, y);
        s_api_touch = false;
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
    board_backlight(s_bright);
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

bool ui_seek(int by_s, int *at_s, int *dur_s)
{
    *at_s = tv_playing() && s_seekable ? play_pos_s() : -1;
    *dur_s = s_play_dur_s;
    if (!by_s || !tv_playing() || !s_seekable || s_play_busy) {
        return false;
    }
    s_seek_s += by_s;
    s_seek_due = esp_timer_get_time(); // ctl_task does it within 100 ms
    return true;
}

bool ui_play(const char *url, const char *title, bool live, const char *id, int dur_s, bool time_seek, int last_pos_s)
{
    s_now_index = -1; // from the web page: no folder to step through
    s_offer_seek = !live && time_seek && id && id[0];
    s_play_dur_s = live ? 0 : dur_s;
    strlcpy(s_play_id, s_offer_seek ? id : "", sizeof(s_play_id));
    return start_play(url, title, live, s_offer_seek ? resume_at(id, last_pos_s, dur_s) : 0);
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
        lv_obj_set_width(s_toast, LV_SIZE_CONTENT);
        lv_label_set_text_fmt(s_toast, "%s  %d%%", volume ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE, volume);
        lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
        s_toast_until = esp_timer_get_time() + 1500000;
        lvgl_port_unlock();
    }
}
