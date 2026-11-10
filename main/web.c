// HTTP API and page, see web.h.

#include "web.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_lvgl_port.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "lvgl.h"

#include "app.h"
#include "board.h"
#include "bridge.h"
#include "h264dec.h"
#include "player.h"
#include "rotate.h"
#include "setup.h"
#include "stream.h"
#include "tv.h"
#include "ui.h"

extern const char index_html[] asm("_binary_index_html_start");
extern const char setup_html[] asm("_binary_setup_html_start");
extern const char chips_html[] asm("_binary_chips_html_start");

// key=... of a query string or form body, percent-decoded into out; false if it isn't there.
static bool form_str(const char *q, const char *key, char *out, size_t len)
{
    char *raw = malloc(len * 3 + 1);
    bool ok = q && raw && httpd_query_key_value(q, key, raw, len * 3 + 1) == ESP_OK;
    if (ok) {
        size_t o = 0;
        for (const char *s = raw; *s && o + 1 < len; s++) {
            if (*s == '%' && s[1] && s[2]) {
                char hex[3] = {s[1], s[2], 0};
                out[o++] = (char)strtol(hex, NULL, 16);
                s += 2;
            } else {
                out[o++] = *s == '+' ? ' ' : *s;
            }
        }
        out[o] = '\0';
    }
    free(raw);
    return ok;
}

// ?key=... percent-decoded into out; false if it isn't there.
static bool query_str(httpd_req_t *req, const char *key, char *out, size_t len)
{
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    char *q = qlen > 1 ? malloc(qlen) : NULL;
    bool ok = q && httpd_req_get_url_query_str(req, q, qlen) == ESP_OK && form_str(q, key, out, len);
    free(q);
    return ok;
}

static int query_int(httpd_req_t *req, const char *key, int fallback)
{
    char v[16];
    return query_str(req, key, v, sizeof(v)) ? atoi(v) : fallback;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, s ? s : "{}");
    free(s);
    return err;
}

static esp_err_t send_raw_json(httpd_req_t *req, const char *s)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, s);
}

static esp_err_t send_page(httpd_req_t *req, const char *html)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, html);
}

// The setup page while the hotspot is open, the status page otherwise.
static esp_err_t page(httpd_req_t *req)
{
    return send_page(req, setup_active() ? setup_html : index_html);
}

static esp_err_t wifi_page(httpd_req_t *req)
{
    return send_page(req, setup_html);
}

// The calibration and the last touch points, raw and mapped.
static void add_touch(cJSON *root)
{
    cJSON *t = cJSON_AddObjectToObject(root, "touch");
    touch_cal_t c;
    board_touch_cal(&c);
    cJSON *cal = cJSON_AddObjectToObject(t, "calibration");
    cJSON_AddBoolToObject(cal, "swap", c.swap);
    cJSON_AddNumberToObject(cal, "ax", c.ax);
    cJSON_AddNumberToObject(cal, "bx", c.bx);
    cJSON_AddNumberToObject(cal, "ay", c.ay);
    cJSON_AddNumberToObject(cal, "by", c.by);
    touch_log_t log[TOUCH_LOG_N];
    int n = board_touch_log(log, TOUCH_LOG_N);
    cJSON *pts = cJSON_AddArrayToObject(t, "points"); // [ms, raw x, raw y, x, y]; x = -1: lifted
    for (int i = 0; i < n; i++) {
        cJSON *p = cJSON_CreateArray();
        cJSON_AddItemToArray(p, cJSON_CreateNumber(log[i].ms));
        cJSON_AddItemToArray(p, cJSON_CreateNumber(log[i].raw_x));
        cJSON_AddItemToArray(p, cJSON_CreateNumber(log[i].raw_y));
        cJSON_AddItemToArray(p, cJSON_CreateNumber(log[i].x));
        cJSON_AddItemToArray(p, cJSON_CreateNumber(log[i].y));
        cJSON_AddItemToArray(pts, p);
    }
}

static void add_board(cJSON *root)
{
    const board_info_t *bi = board_info();
    cJSON *b = cJSON_AddObjectToObject(root, "board");
    cJSON_AddStringToObject(b, "name", bi->name);
    cJSON_AddStringToObject(b, "rev", bi->rev);
    cJSON_AddStringToObject(b, "display", bi->display);
    cJSON_AddStringToObject(b, "touch", bi->touch);
}

// The board, its chips and what they read now, for the chips page.
static esp_err_t chips(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    add_board(root);
    char *buf = malloc(3072);
    if (buf) {
        board_chips_json(buf, 3072);
        cJSON_AddItemToObject(root, "chips", cJSON_Parse(buf));
        free(buf);
    }
    cJSON_AddStringToObject(root, "i2c", board_i2c_devices());
    float a[3];
    if (board_accel(&a[0], &a[1], &a[2])) {
        cJSON *acc = cJSON_AddArrayToObject(root, "accel_g");
        for (int i = 0; i < 3; i++) {
            cJSON_AddItemToArray(acc, cJSON_CreateNumber(round(a[i] * 100.0) / 100));
        }
    }
    cJSON_AddNumberToObject(root, "imu_temp_c", round(board_imu_temp() * 10.0) / 10);
    cJSON_AddNumberToObject(root, "chip_temp_c", round(board_chip_temp() * 10.0) / 10);
    battery_t bat;
    board_battery(&bat);
    cJSON *b = cJSON_AddObjectToObject(root, "battery");
    cJSON_AddNumberToObject(b, "mv", bat.mv);
    cJSON_AddNumberToObject(b, "percent", bat.percent);
    cJSON_AddBoolToObject(b, "charging", bat.charging);
    cJSON_AddBoolToObject(b, "usb", bat.usb);
    cJSON_AddNumberToObject(root, "screen_w", LCD_W);
    cJSON_AddNumberToObject(root, "screen_h", LCD_H);
    add_touch(root);
    return send_json(req, root);
}

static esp_err_t chips_page(httpd_req_t *req)
{
    return send_page(req, chips_html);
}

static esp_err_t status(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "fw", CHANNELSTV_VERSION);
    cJSON_AddStringToObject(root, "idf", esp_app_get_description()->idf_ver);
    cJSON_AddNumberToObject(root, "uptime_s", esp_timer_get_time() / 1000000);
    cJSON_AddStringToObject(root, "reset_reason", app_reset_reason());
    add_board(root);
    const esp_partition_t *run = esp_ota_get_running_partition();
    cJSON_AddStringToObject(root, "partition", run ? run->label : "");
    cJSON_AddBoolToObject(root, "ota_enabled", app_ota_enabled());
    static const char *const modes[] = {"full", "read-only", "off"};
    cJSON_AddStringToObject(root, "web_control", modes[app_web_mode()]);
    net_t net;
    app_net(&net);
    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddBoolToObject(wifi, "connected", net.connected);
    cJSON_AddStringToObject(wifi, "ssid", net.ssid);
    cJSON_AddStringToObject(wifi, "ip", net.ip);
    cJSON_AddNumberToObject(wifi, "rssi", net.rssi);
    battery_t bat;
    board_battery(&bat);
    cJSON *b = cJSON_AddObjectToObject(root, "battery");
    cJSON_AddBoolToObject(b, "present", bat.mv > 0); // the radio firmware's sign of a battery: a reading
    cJSON_AddNumberToObject(b, "mv", bat.mv);
    cJSON_AddNumberToObject(b, "percent", bat.percent);
    cJSON_AddBoolToObject(b, "charging", bat.charging);
    cJSON_AddBoolToObject(b, "usb", bat.usb);
    cJSON_AddNumberToObject(root, "chip_temp_c", board_chip_temp());
    cJSON *ram = cJSON_AddObjectToObject(root, "internal_ram");
    cJSON_AddNumberToObject(ram, "free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(ram, "largest", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(ram, "lowest", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "volume", app_volume());
    cJSON_AddStringToObject(root, "sound", player_state());
    add_touch(root);
    char *buf = malloc(2048);
    if (buf) {
        bridge_status_json(buf, 2048);
        cJSON_AddItemToObject(root, "bridge", cJSON_Parse(buf));
        tv_status_json(buf, 2048);
        cJSON_AddItemToObject(root, "tv", cJSON_Parse(buf));
        free(buf);
    }
    return send_json(req, root);
}

static esp_err_t settings(httpd_req_t *req)
{
    char v[64];
    if (query_str(req, "bridge", v, sizeof(v))) {
        bridge_set_override(v);
    }
    if (query_str(req, "picture", v, sizeof(v))) {
        bridge_set_picture(v);
    }
    if (query_str(req, "size", v, sizeof(v))) {
        int w = 0, h = 0;
        sscanf(v, "%dx%d", &w, &h); // "320x180"; "0x0": the board's own
        bridge_set_size(w, h);
    }
    int vol = query_int(req, "volume", -1);
    if (vol >= 0) {
        app_set_volume(vol);
    }
    int bright = query_int(req, "brightness", -1);
    if (bright >= 0) {
        ui_set_brightness(bright);
    }
    if (query_str(req, "dim", v, sizeof(v))) {
        // "30s", or minutes
        ui_set_dim_seconds(!strcmp(v, "never") ? 0 : strchr(v, 's') ? atoi(v) : atoi(v) * 60);
    }
    if (query_str(req, "off", v, sizeof(v))) {
        ui_set_off_minutes(!strcmp(v, "never") ? 0 : atoi(v));
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "bridge", bridge_override());
    cJSON_AddStringToObject(root, "picture", bridge_picture());
    int pw, ph;
    bridge_size(&pw, &ph);
    char size[16];
    snprintf(size, sizeof(size), "%dx%d", pw, ph);
    cJSON_AddStringToObject(root, "size", size);
    cJSON_AddNumberToObject(root, "volume", app_volume());
    cJSON_AddNumberToObject(root, "brightness", ui_brightness());
    if (!ui_dim_seconds()) {
        cJSON_AddStringToObject(root, "dim", "never");
    } else if (ui_dim_seconds() < 60) {
        cJSON_AddStringToObject(root, "dim", "30s");
    } else {
        cJSON_AddNumberToObject(root, "dim", ui_dim_seconds() / 60);
    }
    if (ui_off_minutes()) {
        cJSON_AddNumberToObject(root, "off", ui_off_minutes());
    } else {
        cJSON_AddStringToObject(root, "off", "never");
    }
    return send_json(req, root);
}

static esp_err_t rescan(httpd_req_t *req)
{
    bridge_rescan();
    return send_raw_json(req, "{\"ok\":true}");
}

// A page of a folder or a search, as JSON: {"total":N,"entries":[...]}
static esp_err_t send_entries(httpd_req_t *req, const cd_entry_t *e, int n, int total)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "total", total);
    cJSON *arr = cJSON_AddArrayToObject(root, "entries");
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", e[i].id);
        cJSON_AddStringToObject(o, "title", e[i].title);
        cJSON_AddBoolToObject(o, "container", e[i].container);
        if (e[i].container) {
            cJSON_AddNumberToObject(o, "children", e[i].children);
        } else {
            cJSON_AddBoolToObject(o, "live", e[i].live);
            cJSON_AddStringToObject(o, "url", e[i].url);
            cJSON_AddNumberToObject(o, "duration_s", e[i].duration_s);
            cJSON_AddBoolToObject(o, "time_seek", e[i].time_seek);
            if (e[i].last_pos_s >= 0) {
                cJSON_AddNumberToObject(o, "last_pos_s", e[i].last_pos_s);
            }
            if (e[i].play_count >= 0) {
                cJSON_AddNumberToObject(o, "play_count", e[i].play_count);
            }
        }
        cJSON_AddItemToArray(arr, o);
    }
    return send_json(req, root);
}

static esp_err_t browse(httpd_req_t *req)
{
    char id[80] = "0";
    query_str(req, "id", id, sizeof(id));
    int start = query_int(req, "start", 0), total = 0;
    cd_entry_t *e = heap_caps_malloc(20 * sizeof(cd_entry_t), MALLOC_CAP_SPIRAM);
    int n = e ? bridge_browse(id, start < 0 ? 0 : start, 20, e, &total) : -1;
    esp_err_t err = n < 0 ? httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "the bridge didn't answer")
                          : send_entries(req, e, n, total);
    heap_caps_free(e);
    return err;
}

// ?q=words[&start=N]: recordings, movies and shows whose titles hold every word, 20 at a time
// (the bridge's UPnP Search); {"no_search":true} if the bridge has none, for the page to search
// by itself.
static esp_err_t search(httpd_req_t *req)
{
    char q[128] = "";
    query_str(req, "q", q, sizeof(q));
    int start = query_int(req, "start", 0), total = 0;
    cd_entry_t *e = heap_caps_malloc(20 * sizeof(cd_entry_t), MALLOC_CAP_SPIRAM);
    int n = e ? bridge_search(q, start < 0 ? 0 : start, 20, e, &total) : -1;
    esp_err_t err = n == -2  ? send_raw_json(req, "{\"no_search\":true,\"total\":0,\"entries\":[]}")
                    : n < 0 ? httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "the bridge didn't answer")
                            : send_entries(req, e, n, total);
    heap_caps_free(e);
    return err;
}

static esp_err_t play(httpd_req_t *req)
{
    char *url = malloc(256), title[96] = "";
    if (!url || !query_str(req, "url", url, 256)) {
        free(url);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "url=");
    }
    query_str(req, "title", title, sizeof(title));
    char why[96];
    if (bridge_profile_problem(why, sizeof(why))) {
        free(url);
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", BRIDGE_PROFILE_HELP);
        cJSON_AddStringToObject(root, "profile_problem", why);
        return send_json(req, root);
    }
    char id[80] = "";
    query_str(req, "id", id, sizeof(id));
    bool ok = ui_play(url, title[0] ? title : url, query_int(req, "live", 0), id, query_int(req, "duration", 0),
                      query_int(req, "seek", 0), query_int(req, "at", -1));
    free(url);
    return send_raw_json(req, ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"another start is under way\"}");
}

// ?set=auto|0..3, ?fake=x,y,z (g) or fake=off; the rotation either way.
static esp_err_t rotation(httpd_req_t *req)
{
    char v[40];
    if (query_str(req, "set", v, sizeof(v))) {
        rotate_set(!strcmp(v, "auto") ? ROTATE_AUTO : atoi(v));
    }
    if (query_str(req, "fake", v, sizeof(v))) {
        float x, y, z;
        bool fake = sscanf(v, "%f,%f,%f", &x, &y, &z) == 3;
        rotate_fake(fake, fake ? x : 0, fake ? y : 0, fake ? z : 0);
    }
    char buf[256];
    rotate_status_json(buf, sizeof(buf));
    return send_raw_json(req, buf);
}

// ?by=±seconds seeks the recording that plays; without it, where it is.
static esp_err_t seek(httpd_req_t *req)
{
    int at, dur;
    bool ok = ui_seek(query_int(req, "by", 0), &at, &dur);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddBoolToObject(root, "seekable", at >= 0);
    cJSON_AddNumberToObject(root, "position_s", at);
    cJSON_AddNumberToObject(root, "duration_s", dur);
    return send_json(req, root);
}

static esp_err_t stop(httpd_req_t *req)
{
    tv_stop();
    return send_raw_json(req, "{\"ok\":true}");
}

// ?gesture=tap|long|up|down|left|right&x=..&y=.. acts as a finger would; reports the last
// touch points either way.
static esp_err_t touch(httpd_req_t *req)
{
    char g[12];
    cJSON *root = cJSON_CreateObject();
    if (query_str(req, "gesture", g, sizeof(g))) {
        cJSON_AddBoolToObject(root, "ok", ui_touch(g, query_int(req, "x", LCD_W / 2), query_int(req, "y", LCD_H / 2)));
    }
    add_touch(root);
    return send_json(req, root);
}

static esp_err_t tv(httpd_req_t *req)
{
    if (query_int(req, "drop", 0)) {
        stream_test_drop();
    }
    char *buf = malloc(2048);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memory");
    }
    tv_status_json(buf, 2048);
    esp_err_t err = send_raw_json(req, buf);
    free(buf);
    return err;
}

// What the screen shows, as a 24-bit BMP: the current frame while something plays (the draw
// task hands over a copy of the next one it draws) or the overlay over it, otherwise the LVGL screen with the top layer
// ("Tuning", toasts) blended over it.
static esp_err_t screen(httpd_req_t *req)
{
    int row = LCD_W * 3, size = 54 + row * LCD_H;
    uint8_t *bmp = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM);
    if (!bmp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memory");
    }
    uint8_t *px = bmp + 54; // rows bottom up, BGR
    int w, h;
    const uint16_t *ov = tv_overlay_shown();
    const uint8_t *yuv = ov ? NULL : tv_capture(&w, &h, 500);
    if (ov) { // the playback controls, the details or a seek over the picture
        for (int y = 0; y < LCD_H; y++) {
            uint8_t *o = px + (LCD_H - 1 - y) * row;
            for (int x = 0; x < LCD_W; x++, o += 3) {
                uint16_t c = ov[y * LCD_W + x];
                c = c >> 8 | c << 8;
                o[0] = (c & 0x1f) << 3;
                o[1] = (c >> 5 & 0x3f) << 2;
                o[2] = (c >> 11) << 3;
            }
        }
    } else if (yuv) {
        uint16_t line[LCD_W * 2];
        int x0 = (LCD_W - w) / 2, y0 = (LCD_H - h) / 2;
        const uint8_t *pu = yuv + w * h, *pv = pu + (w / 2) * (h / 2);
        for (int y = 0; y < h; y += 2) {
            int n = h - y < 2 ? h - y : 2;
            h264_i420_rows_to_rgb565be(yuv + y * w, pu + y / 2 * (w / 2), pv + y / 2 * (w / 2), w, w, n, line);
            for (int k = 0; k < n; k++) {
                uint8_t *o = px + (LCD_H - 1 - (y0 + y + k)) * row + x0 * 3;
                for (int x = 0; x < w; x++, o += 3) {
                    uint16_t c = line[k * w + x];
                    c = c >> 8 | c << 8; // the panel's byte order back to RGB565
                    o[0] = (c & 0x1f) << 3;
                    o[1] = (c >> 5 & 0x3f) << 2;
                    o[2] = (c >> 11) << 3;
                }
            }
        }
    } else if (tv_playing() && tv_first_picture_ms()) {
        heap_caps_free(bmp);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no frame"); // playback holds LVGL
    } else {
        if (!lvgl_port_lock(1000)) {
            heap_caps_free(bmp);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
        }
        lv_draw_buf_t *base = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB888);
        lv_draw_buf_t *top = lv_snapshot_take(lv_layer_top(), LV_COLOR_FORMAT_ARGB8888);
        lvgl_port_unlock();
        for (int y = 0; base && y < LCD_H && y < base->header.h; y++) {
            const uint8_t *b = base->data + y * base->header.stride;
            const uint8_t *t = top && y < top->header.h ? top->data + y * top->header.stride : NULL;
            uint8_t *o = px + (LCD_H - 1 - y) * row;
            for (int x = 0; x < LCD_W && x < base->header.w; x++) {
                int a = t && x < top->header.w ? t[x * 4 + 3] : 0;
                for (int c = 0; c < 3; c++) {
                    o[x * 3 + c] = (b[x * 3 + c] * (255 - a) + (a ? t[x * 4 + c] * a : 0)) / 255;
                }
            }
        }
        if (base) {
            lv_draw_buf_destroy(base);
        }
        if (top) {
            lv_draw_buf_destroy(top);
        }
    }
    bmp[0] = 'B';
    bmp[1] = 'M';
    uint32_t fields[] = {size, 0, 54, 40, LCD_W, LCD_H};
    memcpy(bmp + 2, fields, sizeof(fields));
    bmp[26] = 1;
    bmp[28] = 24;
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, (const char *)bmp, size);
    heap_caps_free(bmp);
    return err;
}

// ---------------------------------------------------------------- wi-fi setup

static esp_err_t wifi_state(httpd_req_t *req)
{
    net_t net;
    app_net(&net);
    char text[96], prev[33] = "";
    setup_status(text, sizeof(text));
    app_previous_wifi(prev, sizeof(prev), NULL, 0);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "hotspot", setup_active());
    cJSON_AddStringToObject(root, "status", text);
    cJSON_AddBoolToObject(root, "failed", setup_failed());
    cJSON_AddBoolToObject(root, "connected", net.connected);
    cJSON_AddStringToObject(root, "ssid", net.ssid);
    cJSON_AddStringToObject(root, "ip", net.ip);
    cJSON_AddStringToObject(root, "previous", prev);
    return send_json(req, root);
}

static esp_err_t wifi_scan(httpd_req_t *req)
{
    char *s = setup_scan_json();
    if (!s) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "the radio is busy, try again");
    }
    esp_err_t err = send_raw_json(req, s);
    free(s);
    return err;
}

// Form body ssid=..&password=.., or previous=1 for the network forgotten last.
static esp_err_t wifi_set(httpd_req_t *req)
{
    char body[256] = "";
    int len = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1, got = 0;
    while (got < len) {
        int n = httpd_req_recv(req, body + got, len - got);
        if (n <= 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
        }
        got += n;
    }
    body[got] = '\0';
    char ssid[33] = "", pass[65] = "", v[4];
    if (form_str(body, "previous", v, sizeof(v)) && v[0] == '1') {
        if (!app_previous_wifi(ssid, sizeof(ssid), pass, sizeof(pass))) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no previous network");
        }
    } else if (!form_str(body, "ssid", ssid, sizeof(ssid)) || !ssid[0]) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid=");
    } else {
        form_str(body, "password", pass, sizeof(pass));
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}"); // before: the hotspot may change channel to follow
    app_set_wifi(ssid, pass);
    return ESP_OK;
}

static void forget_soon(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    app_forget_wifi();
}

static esp_err_t wifi_forget(httpd_req_t *req)
{
    send_raw_json(req, "{\"ok\":true,\"restarting\":true}");
    xTaskCreate(forget_soon, "forget", 3072, NULL, 5, NULL);
    return ESP_OK;
}

// While the hotspot is open, any address the board doesn't know (a phone's captive portal
// check) goes to the setup page.
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t code)
{
    char ip[16], loc[40];
    setup_ap_ip(ip, sizeof(ip));
    if (!ip[0]) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    snprintf(loc, sizeof(loc), "http://%s/", ip);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", loc);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

static void restart_soon(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

// The body is a raw app image (build/channelstv.bin). It goes to the spare slot and the board
// reboots into it; main.c rolls back if the new image doesn't reach Wi-Fi in time.
static esp_err_t ota(httpd_req_t *req)
{
    if (!app_ota_enabled()) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "OTA updates are off: enable them on the board, Settings -> OTA updates");
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part || req->content_len <= 0 || req->content_len > part->size) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
    }
    tv_stop(); // frees Wi-Fi bandwidth and CPU for the upload
    esp_ota_handle_t h;
    if (esp_ota_begin(part, req->content_len, &h) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "begin");
    }
    char *buf = malloc(8192);
    int left = req->content_len;
    while (buf && left > 0) {
        int n = httpd_req_recv(req, buf, left < 8192 ? left : 8192);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0 || esp_ota_write(h, buf, n) != ESP_OK) {
            break;
        }
        left -= n;
    }
    free(buf);
    if (left > 0) {
        esp_ota_abort(h);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload broke off");
    }
    if (esp_ota_end(h) != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "image rejected");
    }
    httpd_resp_sendstr(req, "ok, rebooting\n");
    xTaskCreate(restart_soon, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// ---------------------------------------------------------------- web control

// Who may use each address: anyone, the OTA switch, Web control Full (or the setup hotspot, for
// its own page), or Web control Full alone.
typedef enum { ANYONE, OTA_SWITCH, SETUP, FULL } access_t;
typedef struct {
    esp_err_t (*fn)(httpd_req_t *req);
    access_t access;
} route_t;

static esp_err_t gate(httpd_req_t *req)
{
    const route_t *r = req->user_ctx;
    bool ok = r->access == ANYONE || r->access == OTA_SWITCH || app_web_mode() == WEB_FULL ||
              (r->access == SETUP && setup_active());
    if (!ok) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
                                   "Web control is read-only: change it on the board, Settings -> Web control");
    }
    return r->fn(req);
}

static httpd_handle_t s_server;

void web_apply(void)
{
    bool want = app_web_mode() != WEB_OFF || setup_active();
    if (!want && s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    if (!want || s_server) {
        return;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 26;
    cfg.recv_wait_timeout = 15;
    // a browser keeps several connections open per page: close the idlest instead of refusing
    cfg.lru_purge_enable = true;
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        s_server = NULL;
        return;
    }
    static const struct {
        const char *uri;
        httpd_method_t method;
        route_t route;
    } uris[] = {
        {"/", HTTP_GET, {page, ANYONE}}, // the setup page while the hotspot is open
        {"/api/status", HTTP_GET, {status, ANYONE}},
        {"/chips", HTTP_GET, {chips_page, ANYONE}},
        {"/api/chips", HTTP_GET, {chips, ANYONE}},
        {"/api/ota", HTTP_POST, {ota, OTA_SWITCH}},
        {"/wifi", HTTP_GET, {wifi_page, SETUP}},
        {"/api/wifi", HTTP_GET, {wifi_state, SETUP}},
        {"/api/wifi", HTTP_POST, {wifi_set, SETUP}},
        {"/api/wifi/scan", HTTP_GET, {wifi_scan, SETUP}},
        {"/api/wifi/forget", HTTP_POST, {wifi_forget, SETUP}},
        {"/api/settings", HTTP_GET, {settings, FULL}},
        {"/api/rescan", HTTP_GET, {rescan, FULL}},
        {"/api/browse", HTTP_GET, {browse, FULL}},
        {"/api/search", HTTP_GET, {search, FULL}},
        {"/api/play", HTTP_GET, {play, FULL}},
        {"/api/stop", HTTP_GET, {stop, FULL}},
        {"/api/seek", HTTP_GET, {seek, FULL}},
        {"/api/rotation", HTTP_GET, {rotation, FULL}},
        {"/api/tv", HTTP_GET, {tv, FULL}},
        {"/api/touch", HTTP_GET, {touch, FULL}},
        {"/api/screen", HTTP_GET, {screen, FULL}},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_uri_t u = {.uri = uris[i].uri, .method = uris[i].method, .handler = gate,
                         .user_ctx = (void *)&uris[i].route};
        httpd_register_uri_handler(s_server, &u);
    }
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, not_found);
}
