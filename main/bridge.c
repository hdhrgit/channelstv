// Finding and browsing the bridge, see bridge.h.

#include "bridge.h"
#include "board.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include "nvs.h"

#define NVS_NS        "channelstv"
#define DEFAULT_PORT  8200
#define DESC_MAX      (16 * 1024)
#define SOAP_MAX      (96 * 1024)  // a page of 15 items is ~15 KB, escaped
#define MAX_RESPONDERS 12
#define RETRY_S       30
#define USER_AGENT    "channelstv/" CHANNELSTV_VERSION

static const char *TAG = "bridge";

static bool (*s_online)(void);
static SemaphoreHandle_t s_lock;
static volatile bridge_state_t s_state = BRIDGE_WAITING;
static char s_base[96];       // http://host:port
static char s_control[160];   // the ContentDirectory control URL
static char s_name[64];
static char s_udn[64];
static const char *s_found_by = "";
static char s_override[64];
static bool s_4x3;
static bool s_full; // the picture fills the larger panel: its middle, cropped to the panel's shape
static int s_fps;
static int s_scale = 10;
static volatile unsigned s_gen;
static volatile bool s_rescan = true;
// The bridge's client profile the board asks for, and the overrides it adds for the centre 4:3 on
// the 240x240 panel: crop43, which every bridge with the profile knows
#define PROFILE "esp32"
#define OVERRIDES_4X3 "w=240&h=192&fit=crop43"
// What is wrong with that profile ("" when it's fine or couldn't be read), from its /api/state
static char s_problem[96];
static char s_bversion[24]; // the bridge's own version, from its /api/state
static bool s_too_old;      // s_problem is that version: older than BRIDGE_MIN_VERSION
static volatile bool s_recheck;
static TaskHandle_t s_task;

// ---------------------------------------------------------------- settings

static void nvs_str(const char *key, char *out, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = len;
        if (nvs_get_str(h, key, out, &n) != ESP_OK) {
            out[0] = '\0';
        }
        nvs_close(h);
    }
}

static void nvs_put(const char *key, const char *value)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, key, value);
        nvs_commit(h);
        nvs_close(h);
    }
}

const char *bridge_override(void)
{
    return s_override;
}

void bridge_set_override(const char *host_port)
{
    char v[sizeof(s_override)];
    // "http://192.0.2.10:8200/" is taken as well as "192.0.2.10:8200"
    const char *p = strstr(host_port, "://") ? strstr(host_port, "://") + 3 : host_port;
    strlcpy(v, p, sizeof(v));
    v[strcspn(v, "/ \t\r\n")] = '\0';
    strlcpy(s_override, v, sizeof(s_override));
    nvs_put("override", s_override);
    bridge_rescan();
}

const char *bridge_picture(void)
{
    return s_full ? "full" : s_4x3 ? "4x3" : "16x9";
}

void bridge_set_picture(const char *shape)
{
    s_4x3 = !strcmp(shape, "4x3");
    s_full = !strcmp(shape, "full") && LCD_W * LCD_H > 240 * 240;
    nvs_put("picture", bridge_picture());
}

// The picture size asked of the bridge on a panel bigger than the profile's 240 wide (0: the
// profile's own), until a restart.
static int s_pic_w, s_pic_h;

void bridge_set_size(int w, int h)
{
    s_pic_w = w > 0 && w <= LCD_W ? w & ~1 : 0;
    s_pic_h = h > 0 && h <= LCD_H ? h & ~1 : 0;
}

void bridge_size(int *w, int *h)
{
    bool big = LCD_W > 240;
    // the panel's width (VideoJet holds 24 fps there, upright and stretched sideways)
    // the screen as the picture will be drawn: across the panel's height when the video turns
    bool side = (board_video_rotation() & 1) && LCD_W != LCD_H;
    int sw = side ? LCD_H : LCD_W, sh = side ? LCD_W : LCD_H;
    *w = s_pic_w ? s_pic_w : big ? (s_4x3 ? 320 : sw) : 0;
    *h = s_pic_h ? s_pic_h : big ? (s_4x3 ? 240 : sh) : 0;
    if (!s_pic_w && big && bridge_scale() > 10) {
        int sw = (*w * 10 / bridge_scale()) & ~7; // a multiple of 8 for the PIE path
        *h = (*h * sw / *w) & ~1;
        *w = sw;
    }
}

// ---------------------------------------------------------------- HTTP

// GETs url into buf (NUL-terminated). Returns its length, or -1 unless it came back 200.
static int http_get(const char *url, char *buf, int cap, int timeout_ms)
{
    esp_http_client_config_t cfg = {.url = url, .timeout_ms = timeout_ms, .buffer_size = 2048, .buffer_size_tx = 512,
                                    .user_agent = USER_AGENT};
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    int got = -1;
    if (c && esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        if (esp_http_client_get_status_code(c) == 200) {
            int r;
            got = 0;
            while (got < cap - 1 && (r = esp_http_client_read(c, buf + got, cap - 1 - got)) > 0) {
                got += r;
            }
            buf[got] = '\0';
        }
    }
    if (c) {
        esp_http_client_cleanup(c);
    }
    return got;
}

// The text of the first <tag>...</tag> in [s, end) into out; false if there is none.
static bool tag_text(const char *s, const char *end, const char *tag, char *out, size_t len)
{
    char open[48], close[48];
    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    const char *a = strstr(s, open);
    if (!a || (end && a >= end)) {
        // with attributes: <tag ...>
        snprintf(open, sizeof(open), "<%s ", tag);
        a = strstr(s, open);
        if (!a || (end && a >= end) || !(a = strchr(a, '>'))) {
            return false;
        }
        a++;
    } else {
        a += strlen(open);
    }
    const char *b = strstr(a, close);
    if (!b || (end && b > end)) {
        return false;
    }
    size_t n = b - a < (int)len - 1 ? (size_t)(b - a) : len - 1;
    memcpy(out, a, n);
    out[n] = '\0';
    while (n && (out[n - 1] == ' ' || out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == '\t')) {
        out[--n] = '\0';
    }
    const char *t = out;
    while (*t == ' ' || *t == '\n' || *t == '\r' || *t == '\t') {
        t++;
    }
    memmove(out, t, strlen(t) + 1);
    return true;
}

// XML entities back to characters, in place.
static void unescape(char *s)
{
    static const struct { const char *e; char c; } ents[] = {
        {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&amp;", '&'}};
    char *o = s;
    for (char *p = s; *p;) {
        bool done = false;
        if (*p == '&') {
            for (size_t i = 0; i < sizeof(ents) / sizeof(ents[0]); i++) {
                size_t n = strlen(ents[i].e);
                if (!strncmp(p, ents[i].e, n)) {
                    *o++ = ents[i].c;
                    p += n;
                    done = true;
                    break;
                }
            }
            if (!done && p[1] == '#') { // &#39; and the like, ASCII only
                char *semi = strchr(p, ';');
                long v = semi ? (p[2] == 'x' ? strtol(p + 3, NULL, 16) : strtol(p + 2, NULL, 10)) : 0;
                if (semi && v > 0 && v < 128 && semi - p < 8) {
                    *o++ = (char)v;
                    p = semi + 1;
                    done = true;
                }
            }
        }
        if (!done) {
            *o++ = *p++;
        }
    }
    *o = '\0';
}

// "http://host:port" of a URL
static void url_base(const char *url, char *out, size_t len)
{
    const char *host = strstr(url, "://");
    host = host ? host + 3 : url;
    const char *slash = strchr(host, '/');
    size_t n = slash ? (size_t)(slash - url) : strlen(url);
    n = n < len - 1 ? n : len - 1;
    memcpy(out, url, n);
    out[n] = '\0';
}

// ---------------------------------------------------------------- the device

typedef struct {
    char name[64], udn[64], control[160], base[96];
} device_t;

// Fetches a device description and accepts it only if it is the bridge's.
static bool check_device(const char *location, device_t *dev, char *buf)
{
    if (http_get(location, buf, DESC_MAX, 4000) <= 0) {
        return false;
    }
    char murl[96];
    if (!tag_text(buf, NULL, "manufacturerURL", murl, sizeof(murl)) || strcmp(murl, BRIDGE_MANUFACTURER_URL)) {
        return false;
    }
    memset(dev, 0, sizeof(*dev));
    tag_text(buf, NULL, "friendlyName", dev->name, sizeof(dev->name));
    unescape(dev->name);
    tag_text(buf, NULL, "UDN", dev->udn, sizeof(dev->udn));
    url_base(location, dev->base, sizeof(dev->base));
    char ctl[128] = "/ctl/ContentDirectory";
    const char *svc = strstr(buf, "service:ContentDirectory:1");
    if (svc) {
        tag_text(svc, NULL, "controlURL", ctl, sizeof(ctl));
    }
    if (!strncmp(ctl, "http", 4)) {
        strlcpy(dev->control, ctl, sizeof(dev->control));
    } else {
        snprintf(dev->control, sizeof(dev->control), "%s%s%s", dev->base, ctl[0] == '/' ? "" : "/", ctl);
    }
    return true;
}

// The bridge's own status page answers: it's really the bridge, and it's up.
static bool check_state(const char *base, char *buf)
{
    char url[128];
    snprintf(url, sizeof(url), "%s/api/state", base);
    return http_get(url, buf, DESC_MAX, 4000) > 0 && strstr(buf, "\"version\"") && strstr(buf, "\"dvr\"");
}

static void use(const device_t *dev, const char *by)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_base, dev->base, sizeof(s_base));
    strlcpy(s_control, dev->control, sizeof(s_control));
    strlcpy(s_name, dev->name, sizeof(s_name));
    strlcpy(s_udn, dev->udn, sizeof(s_udn));
    s_found_by = by;
    s_state = BRIDGE_FOUND;
    s_gen++;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "bridge \"%s\" at %s (%s)", dev->name, dev->base, by);
}

// ---------------------------------------------------------------- the bridge's client profile

// What a client profile must be for the board to decode its streams; "" if it is.
static const cJSON *client_named(const cJSON *clients, const char *name)
{
    const cJSON *c, *found = NULL;
    cJSON_ArrayForEach(c, clients) {
        const cJSON *n = cJSON_GetObjectItem(c, "name");
        if (cJSON_IsString(n) && !strcmp(n->valuestring, name)) {
            found = c;
        }
    }
    return found;
}

// found: the client the bridge uses for name (NULL if none).
static void profile_problem(const cJSON *found, const char *name, char *out, size_t len)
{
    const cJSON *p = cJSON_GetObjectItem(found, "profile");
    const cJSON *mode = cJSON_GetObjectItem(p, "mode"), *t = cJSON_GetObjectItem(p, "transcode");
    const cJSON *h264 = cJSON_GetObjectItem(t, "h264_profile"), *w = cJSON_GetObjectItem(t, "max_width");
    if (!found) {
        snprintf(out, len, "no %s client", name);
    } else if (!cJSON_IsString(mode) || strcmp(mode->valuestring, "transcode")) {
        snprintf(out, len, "%s: mode %s, needs transcode", name, cJSON_IsString(mode) ? mode->valuestring : "unset");
    } else if (!cJSON_IsString(h264) || strcmp(h264->valuestring, "baseline")) {
        snprintf(out, len, "%s: H.264 %s, needs baseline", name, cJSON_IsString(h264) ? h264->valuestring : "unset");
    } else if (!cJSON_IsTrue(cJSON_GetObjectItem(t, "single_slice"))) {
        snprintf(out, len, "%s: needs single slice", name);
    } else if (!cJSON_IsNumber(w) || w->valueint <= 0 || w->valueint > 240) {
        snprintf(out, len, "%s: width must be 240 or less", name);
    } else if (len) {
        out[0] = '\0';
    }
}

// True if version ("0.99.11", "v0.99.11+2.gf19dd58") is older than BRIDGE_MIN_VERSION, by its first
// three numbers only (0.99.12+3.gabc counts as 0.99.12); false if it can't be read as one
static bool version_too_old(const char *version)
{
    int a[3], m[3];
    if (*version == 'v') {
        version++;
    }
    if (sscanf(version, "%d.%d.%d", &a[0], &a[1], &a[2]) != 3 ||
        sscanf(BRIDGE_MIN_VERSION, "%d.%d.%d", &m[0], &m[1], &m[2]) != 3) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        if (a[i] != m[i]) {
            return a[i] < m[i];
        }
    }
    return false;
}

// Reads the client profile from the bridge's /api/state (the board never writes to it): one in
// its config if there is one, which the bridge uses first, else the one built into the bridge.
// If the state can't be read, nothing is held against the profile.
static void check_profiles(const char *base, char *buf)
{
    char url[128], problem[96] = "";
    snprintf(url, sizeof(url), "%s/api/state", base);
    cJSON *root = http_get(url, buf, DESC_MAX, 4000) > 0 ? cJSON_Parse(buf) : NULL;
    const cJSON *clients = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "config"), "clients");
    const cJSON *builtin = cJSON_GetObjectItem(root, "builtin");
    const cJSON *ver = cJSON_GetObjectItem(root, "version");
    if (cJSON_IsString(ver)) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(s_bversion, ver->valuestring, sizeof(s_bversion));
        xSemaphoreGive(s_lock);
    }
    if (root && cJSON_GetObjectItem(root, "config")) {
        const cJSON *found = client_named(clients, PROFILE);
        profile_problem(found ? found : client_named(builtin, PROFILE), PROFILE, problem, sizeof(problem));
    } else {
        ESP_LOGW(TAG, "can't read the bridge's client profiles");
    }
    char version[24] = "";
    if (cJSON_IsString(ver)) {
        strlcpy(version, ver->valuestring, sizeof(version));
    }
    cJSON_Delete(root);
    // one that can't fit the picture would play it squashed or cut wrong: none at all instead.
    // Its /api/caps lists the fits it knows; a bridge without that goes by its version.
    bool old = false;
    if (!problem[0] && LCD_W > 240) { // only the larger panel asks for these fits
        snprintf(url, sizeof(url), "%s/api/caps", base);
        cJSON *caps = http_get(url, buf, DESC_MAX, 4000) > 0 ? cJSON_Parse(buf) : NULL;
        const cJSON *fits = cJSON_GetObjectItem(caps, "fits");
        if (cJSON_IsArray(fits)) {
            bool w = false, h = false;
            const cJSON *f;
            cJSON_ArrayForEach(f, fits)
            {
                w = w || (cJSON_IsString(f) && !strcmp(f->valuestring, "width"));
                h = h || (cJSON_IsString(f) && !strcmp(f->valuestring, "height"));
            }
            old = !w || !h;
        } else {
            old = version_too_old(version);
        }
        cJSON_Delete(caps);
    }
    if (old) {
        snprintf(problem, sizeof(problem), "the bridge is %.40s", version[0] ? version : "older");
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_problem, problem, sizeof(s_problem));
    s_too_old = old;
    xSemaphoreGive(s_lock);
    if (problem[0]) {
        ESP_LOGW(TAG, "bridge profile: %s", problem);
    }
}

void bridge_version(char *out, size_t len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(out, s_bversion, len);
    xSemaphoreGive(s_lock);
}

bool bridge_profile_problem(char *out, size_t len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(out, s_problem, len);
    xSemaphoreGive(s_lock);
    return out[0];
}

const char *bridge_problem_help(void)
{
    return s_too_old ? BRIDGE_UPDATE_HELP : BRIDGE_PROFILE_HELP;
}

bool bridge_recheck_profiles(void)
{
    char base[96], *buf = heap_caps_malloc(DESC_MAX, MALLOC_CAP_SPIRAM);
    bridge_base(base, sizeof(base));
    if (buf && base[0]) {
        check_profiles(base, buf);
    }
    heap_caps_free(buf);
    char p[96];
    return bridge_profile_problem(p, sizeof(p));
}

static void lose(bridge_state_t why)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool had = s_base[0];
    s_base[0] = s_control[0] = s_name[0] = '\0';
    s_found_by = "";
    s_problem[0] = '\0';
    s_too_old = false;
    s_bversion[0] = '\0';
    s_state = why;
    if (had) {
        s_gen++;
    }
    xSemaphoreGive(s_lock);
}

// SSDP: the LOCATIONs of MediaServer:1 devices whose SERVER header names the bridge's
// "dlna/<version>" token. Two M-SEARCHes, answers collected for 3 s.
static int ssdp_search(char loc[][160], int max)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return 0;
    }
    uint8_t ttl = 2;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    struct timeval tv = {.tv_sec = 0, .tv_usec = 300000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(1900)};
    to.sin_addr.s_addr = inet_addr("239.255.255.250");
    static const char msg[] = "M-SEARCH * HTTP/1.1\r\n"
                              "HOST: 239.255.255.250:1900\r\n"
                              "MAN: \"ssdp:discover\"\r\n"
                              "MX: 2\r\n"
                              "ST: urn:schemas-upnp-org:device:MediaServer:1\r\n"
                              "USER-AGENT: " USER_AGENT "\r\n\r\n";
    int n = 0;
    char *buf = heap_caps_malloc(1536, MALLOC_CAP_SPIRAM);
    for (int round = 0; round < 2 && buf; round++) {
        sendto(sock, msg, sizeof(msg) - 1, 0, (struct sockaddr *)&to, sizeof(to));
        for (int i = 0; i < 5; i++) { // 1.5 s each round
            int r = recv(sock, buf, 1535, 0);
            if (r <= 0) {
                continue;
            }
            buf[r] = '\0';
            char *server = strcasestr(buf, "\nSERVER:"), *l = strcasestr(buf, "\nLOCATION:");
            if (!l || !server) {
                continue;
            }
            char line[128];
            strlcpy(line, server + 8, sizeof(line));
            line[strcspn(line, "\r\n")] = '\0';
            if (!strstr(line, " dlna/")) {
                continue;
            }
            l += 10;
            while (*l == ' ') {
                l++;
            }
            char url[160];
            strlcpy(url, l, sizeof(url));
            url[strcspn(url, "\r\n")] = '\0';
            bool seen = false;
            for (int k = 0; k < n && !seen; k++) {
                seen = !strcmp(loc[k], url);
            }
            if (!seen && n < max) {
                strlcpy(loc[n++], url, sizeof(loc[0]));
            }
        }
    }
    heap_caps_free(buf);
    close(sock);
    return n;
}

static bool find(char *buf)
{
    device_t dev;
    if (s_override[0]) {
        // by hand: that address, whatever it says about itself
        char base[96], desc[128];
        snprintf(base, sizeof(base), "http://%s%s", s_override, strchr(s_override, ':') ? "" : ":8200");
        snprintf(desc, sizeof(desc), "%s/description.xml", base);
        if (!check_device(desc, &dev, buf)) {
            if (!check_state(base, buf)) {
                lose(BRIDGE_UNREACHABLE);
                return false;
            }
            memset(&dev, 0, sizeof(dev));
            strlcpy(dev.base, base, sizeof(dev.base));
            snprintf(dev.control, sizeof(dev.control), "%s/ctl/ContentDirectory", base);
        }
        strlcpy(dev.base, base, sizeof(dev.base)); // the address given, not where its description says
        use(&dev, "manual");
        return true;
    }
    // where it was last time
    char loc[160], udn[64];
    nvs_str("loc", loc, sizeof(loc));
    nvs_str("udn", udn, sizeof(udn));
    if (loc[0] && check_device(loc, &dev, buf) && !strcmp(dev.udn, udn) && check_state(dev.base, buf)) {
        use(&dev, "saved");
        return true;
    }
    s_state = BRIDGE_SEARCHING;
    static char found[MAX_RESPONDERS][160];
    int n = ssdp_search(found, MAX_RESPONDERS);
    ESP_LOGI(TAG, "%d SSDP answers that could be the bridge", n);
    for (int i = 0; i < n; i++) {
        if (check_device(found[i], &dev, buf) && check_state(dev.base, buf)) {
            nvs_put("loc", found[i]);
            nvs_put("udn", dev.udn);
            use(&dev, "discovery");
            return true;
        }
    }
    lose(BRIDGE_NOT_FOUND);
    return false;
}

static void bridge_task(void *arg)
{
    char *buf = heap_caps_malloc(DESC_MAX, MALLOC_CAP_SPIRAM);
    int wait = 0;
    while (true) {
        if (!s_online()) {
            if (s_state != BRIDGE_WAITING) {
                lose(BRIDGE_WAITING);
            }
            s_rescan = true;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        bool due = s_rescan || (s_state != BRIDGE_FOUND && wait <= 0);
        if (due && buf) {
            s_rescan = false;
            if (s_state != BRIDGE_FOUND) {
                s_state = BRIDGE_SEARCHING;
            }
            if (find(buf)) {
                wait = 0;
                s_recheck = true;
            } else {
                wait = RETRY_S;
            }
        }
        if (s_recheck && s_state == BRIDGE_FOUND && buf) {
            s_recheck = false;
            char base[96];
            bridge_base(base, sizeof(base));
            check_profiles(base, buf);
            s_gen++; // the screen says it, if a profile is wrong
        }
        // woken early by bridge_rescan()
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        wait--;
    }
}

int bridge_scale(void)
{
    return LCD_W * LCD_H > 240 * 240 ? s_scale : 10;
}

void bridge_set_scale(int tenths)
{
    s_scale = tenths == 15 || tenths == 20 ? tenths : 10;
    char v[4];
    snprintf(v, sizeof(v), "%d", s_scale);
    nvs_put("scale", v);
}

static int fps_default(void)
{
    return LCD_W * LCD_H > 240 * 240 ? 15 : 24;
}

int bridge_fps(void)
{
    return s_fps;
}

void bridge_set_fps(int fps)
{
    s_fps = fps == 15 || fps == 20 || fps == 24 ? fps : fps_default();
    char v[4];
    snprintf(v, sizeof(v), "%d", s_fps);
    nvs_put("fps", v);
}

void bridge_start(bool (*online)(void))
{
    s_online = online;
    s_lock = xSemaphoreCreateMutex();
    nvs_str("override", s_override, sizeof(s_override));
    char p[8] = "";
    nvs_str("picture", p, sizeof(p));
    s_4x3 = !strcmp(p, "4x3");
    s_full = !strcmp(p, "full") && LCD_W * LCD_H > 240 * 240;
    nvs_str("scale", p, sizeof(p));
    s_scale = atoi(p) == 15 || atoi(p) == 20 ? atoi(p) : 10;
    nvs_str("fps", p, sizeof(p));
    int fps = atoi(p); // an earlier "0" (the bridge's own rate) gets the default
    s_fps = fps == 15 || fps == 20 || fps == 24 ? fps : fps_default();
    // internal stack: it writes NVS (flash)
    xTaskCreate(bridge_task, "bridge", 6144, NULL, 3, &s_task);
}

void bridge_rescan(void)
{
    s_rescan = true;
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

bridge_state_t bridge_state(void)
{
    return s_state;
}

const char *bridge_state_text(void)
{
    switch (s_state) {
    case BRIDGE_WAITING: return "waiting for Wi-Fi";
    case BRIDGE_SEARCHING: return "searching";
    case BRIDGE_FOUND: return "found";
    case BRIDGE_NOT_FOUND: return "not found";
    case BRIDGE_UNREACHABLE: return "not reachable";
    }
    return "";
}

void bridge_base(char *out, size_t len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(out, s_base, len);
    xSemaphoreGive(s_lock);
}

const char *bridge_name(void)
{
    return s_name;
}

const char *bridge_found_by(void)
{
    return s_found_by;
}

unsigned bridge_gen(void)
{
    return s_gen;
}

// ---------------------------------------------------------------- ContentDirectory

// The stream URL with ?profile=esp32 unless it names a profile already, for the centre 4:3 the
// bridge's overrides for that picture, and the frame rate.
static void with_profile(const char *url, char *out, size_t len)
{
    strlcpy(out, url, len);
    if (!strstr(url, "profile=")) {
        snprintf(out, len, "%s%cprofile=" PROFILE, url, strchr(url, '?') ? '&' : '?');
    }
    int w, h;
    bridge_size(&w, &h);
    if (w && h && !strstr(out, "fit=")) {
        size_t n = strlen(out);
        // the bridge fits the picture into w x h by its own shape: the whole of it (width), or its
        // middle cropped to w x h (height)
        snprintf(out + n, len - n, "%cw=%d&h=%d&fit=%s", strchr(out, '?') ? '&' : '?', w, h,
                 s_4x3 || s_full || s_pic_w ? "height" : "width");
    } else if (s_4x3 && !strstr(out, "fit=")) {
        size_t n = strlen(out);
        snprintf(out + n, len - n, "%c" OVERRIDES_4X3, strchr(out, '?') ? '&' : '?');
    }
    if (!strstr(out, "fps=")) {
        size_t n = strlen(out);
        snprintf(out + n, len - n, "%cfps=%d", strchr(out, '?') ? '&' : '?', s_fps);
    }
}

// The value of attr="..." in an element's start tag [s, end).
static bool attr(const char *s, const char *end, const char *name, char *out, size_t len)
{
    char key[32];
    snprintf(key, sizeof(key), " %s=\"", name);
    const char *a = strstr(s, key);
    if (!a || a >= end) {
        return false;
    }
    a += strlen(key);
    const char *b = strchr(a, '"');
    if (!b || b > end) {
        return false;
    }
    size_t n = b - a < (int)len - 1 ? (size_t)(b - a) : len - 1;
    memcpy(out, a, n);
    out[n] = '\0';
    return true;
}

// A DIDL time, "H:MM:SS(.mmm)", "M:SS" or plain seconds ("610.5"), in whole seconds; -1 if
// it is none of these.
static int didl_seconds(const char *s)
{
    int part[3], n = 0;
    const char *p = s;
    while (*p == ' ') {
        p++;
    }
    if (!*p) {
        return -1;
    }
    while (n < 3) {
        char *end;
        double v = strtod(p, &end);
        if (end == p || v < 0) {
            return -1;
        }
        part[n++] = (int)v;
        if (*end != ':') {
            p = end;
            break;
        }
        p = end + 1;
    }
    if (*p && *p != ' ') {
        return -1;
    }
    return n == 3 ? part[0] * 3600 + part[1] * 60 + part[2] : n == 2 ? part[0] * 60 + part[1] : part[0];
}

// The DIDL-Lite of a Browse result: containers and items, in order.
static int parse_didl(const char *d, cd_entry_t *out, int max)
{
    int n = 0;
    for (const char *p = d; n < max;) {
        const char *c = strstr(p, "<container "), *i = strstr(p, "<item ");
        const char *s = c && (!i || c < i) ? c : i;
        if (!s) {
            break;
        }
        bool container = s == c;
        const char *start_end = strchr(s, '>');
        const char *end = strstr(s, container ? "</container>" : "</item>");
        if (!start_end || !end) {
            break;
        }
        cd_entry_t *e = &out[n];
        memset(e, 0, sizeof(*e));
        e->container = container;
        e->children = -1;
        e->last_pos_s = e->play_count = -1;
        attr(s, start_end, "id", e->id, sizeof(e->id));
        unescape(e->id);
        char cc[12];
        if (attr(s, start_end, "childCount", cc, sizeof(cc))) {
            e->children = atoi(cc);
        }
        tag_text(start_end, end, "dc:title", e->title, sizeof(e->title));
        unescape(e->title);
        char cls[96] = "";
        tag_text(start_end, end, "upnp:class", cls, sizeof(cls));
        e->live = strstr(cls, "videoBroadcast") != NULL;
        if (!container) {
            char url[224] = "", dur[24], info[160];
            // <res protocolInfo="...DLNA.ORG_OP=10;..." duration="H:MM:SS.mmm">
            const char *res = strstr(start_end, "<res"), *res_end = res ? strchr(res, '>') : NULL;
            if (res && res_end && res_end < end) {
                int h = 0, m = 0, sec = 0;
                if (attr(res, res_end, "duration", dur, sizeof(dur)) && sscanf(dur, "%d:%d:%d", &h, &m, &sec) == 3) {
                    e->duration_s = h * 3600 + m * 60 + sec;
                }
                const char *op = attr(res, res_end, "protocolInfo", info, sizeof(info)) ? strstr(info, "DLNA.ORG_OP=") : NULL;
                e->time_seek = op && op[12] == '1'; // the first digit: time seek; the second: byte ranges
            }
            char v[24];
            if (tag_text(start_end, end, "upnp:lastPlaybackPosition", v, sizeof(v))) {
                e->last_pos_s = didl_seconds(v);
            }
            if (tag_text(start_end, end, "upnp:playbackCount", v, sizeof(v)) && didl_seconds(v) >= 0) {
                e->play_count = atoi(v);
            }
            tag_text(start_end, end, "res", url, sizeof(url));
            unescape(url);
            if (url[0]) {
                with_profile(url, e->url, sizeof(e->url));
            }
        }
        if (e->id[0] && (container || e->url[0])) {
            n++;
        }
        p = end + 1;
    }
    return n;
}

// A ContentDirectory action whose answer is a page of DIDL-Lite (Browse, Search), with args its
// XML arguments. Returns how many came back and sets *total, or -1; *status is the HTTP status
// (500 with a UPnP fault, such as an action the bridge doesn't have).
static int cd_call(const char *action, const char *args, int max, cd_entry_t *out, int *total, int *status)
{
    char control[200];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // the profile on the request too: the bridge then hands out stream URLs made for it
    snprintf(control, sizeof(control), "%s%sprofile=" PROFILE, s_control, strchr(s_control, '?') ? "&" : "?");
    bool have = s_control[0];
    xSemaphoreGive(s_lock);
    *total = 0;
    *status = 0;
    if (!have) {
        return -1;
    }
    char *body = malloc(1536), soapaction[96];
    int blen = body ? snprintf(body, 1536,
                               "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                               "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                               "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
                               "<u:%s xmlns:u=\"urn:schemas-upnp-org:service:ContentDirectory:1\">%s</u:%s>"
                               "</s:Body></s:Envelope>",
                               action, args, action) : 0;
    snprintf(soapaction, sizeof(soapaction), "\"urn:schemas-upnp-org:service:ContentDirectory:1#%s\"", action);
    esp_http_client_config_t cfg = {.url = control, .method = HTTP_METHOD_POST, .timeout_ms = 10000,
                                    .buffer_size = 4096, .buffer_size_tx = 1024, .user_agent = USER_AGENT};
    esp_http_client_handle_t c = body && blen < 1536 ? esp_http_client_init(&cfg) : NULL;
    char *resp = heap_caps_malloc(SOAP_MAX, MALLOC_CAP_SPIRAM);
    int n = -1;
    if (c && resp) {
        esp_http_client_set_header(c, "Content-Type", "text/xml; charset=\"utf-8\"");
        esp_http_client_set_header(c, "SOAPACTION", soapaction);
        if (esp_http_client_open(c, blen) == ESP_OK && esp_http_client_write(c, body, blen) == blen) {
            esp_http_client_fetch_headers(c);
            int got = 0, r;
            while (got < SOAP_MAX - 1 && (r = esp_http_client_read(c, resp + got, SOAP_MAX - 1 - got)) > 0) {
                got += r;
            }
            resp[got] = '\0';
            *status = esp_http_client_get_status_code(c);
            char *res = strstr(resp, "<Result>"), *res_end = res ? strstr(res, "</Result>") : NULL;
            if (*status == 200 && res && res_end) {
                char tm[16] = "0";
                tag_text(resp, NULL, "TotalMatches", tm, sizeof(tm));
                *total = atoi(tm);
                *res_end = '\0';
                unescape(res + 8); // the DIDL-Lite document, escaped inside the SOAP answer
                n = parse_didl(res + 8, out, max);
            } else {
                ESP_LOGW(TAG, "%s: HTTP %d", action, *status);
            }
        }
    }
    if (c) {
        esp_http_client_cleanup(c);
    }
    free(body);
    heap_caps_free(resp);
    return n;
}

// text with XML's &, < and > escaped (and, with quoted, " and \\ escaped for a search string)
static void xml_escape(const char *in, char *out, size_t len, bool quoted)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 7 < len; p++) {
        if (*p == '&') {
            o += snprintf(out + o, len - o, "&amp;");
        } else if (*p == '<') {
            o += snprintf(out + o, len - o, "&lt;");
        } else if (*p == '>') {
            o += snprintf(out + o, len - o, "&gt;");
        } else if (quoted && (*p == '"' || *p == '\\')) {
            out[o++] = '\\';
            out[o++] = *p;
        } else {
            out[o++] = *p;
        }
    }
    out[o] = '\0';
}

int bridge_browse(const char *id, int start, int max, cd_entry_t *out, int *total)
{
    char esc_id[160], args[384];
    xml_escape(id, esc_id, sizeof(esc_id), false);
    snprintf(args, sizeof(args),
             "<ObjectID>%s</ObjectID><BrowseFlag>BrowseDirectChildren</BrowseFlag><Filter>*</Filter>"
             "<StartingIndex>%d</StartingIndex><RequestedCount>%d</RequestedCount><SortCriteria></SortCriteria>",
             esc_id, start, max);
    int status, n = cd_call("Browse", args, max, out, total, &status);
    if (n < 0) {
        ESP_LOGW(TAG, "Browse %s failed", id);
        bridge_rescan();
    }
    return n;
}

int bridge_search(const char *words, int start, int max, cd_entry_t *out, int *total)
{
    // every word in the title: dc:title contains "a" and dc:title contains "b" ...
    char crit[640] = "", word[64], esc[160];
    const char *p = words;
    while (*p) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        size_t n = strcspn(p, " \t");
        if (!n) {
            break;
        }
        snprintf(word, sizeof(word), "%.*s", (int)n, p);
        p += n;
        char quoted[160];
        xml_escape(word, quoted, sizeof(quoted), true); // escaped for the criteria, then for XML
        xml_escape(quoted, esc, sizeof(esc), false);
        size_t at = strlen(crit);
        snprintf(crit + at, sizeof(crit) - at, "%sdc:title contains &quot;%s&quot;", at ? " and " : "", esc);
    }
    if (!crit[0]) {
        *total = 0;
        return 0;
    }
    // recordings, movies and shows: not live channels (object.item.videoItem.videoBroadcast)
    size_t at = strlen(crit);
    snprintf(crit + at, sizeof(crit) - at,
             " and (upnp:class = &quot;object.item.videoItem&quot; or upnp:class derivedfrom &quot;object.container&quot;)");
    char *args = malloc(1024);
    if (!args) {
        return -1;
    }
    snprintf(args, 1024,
             "<ContainerID>0</ContainerID><SearchCriteria>%s</SearchCriteria><Filter>*</Filter>"
             "<StartingIndex>%d</StartingIndex><RequestedCount>%d</RequestedCount><SortCriteria></SortCriteria>",
             crit, start, max);
    int status, n = cd_call("Search", args, max, out, total, &status);
    free(args);
    return n < 0 && status == 500 ? -2 : n;
}

void bridge_status_json(char *out, size_t len)
{
    char base[96], problem[96], ver[24];
    bridge_base(base, sizeof(base));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(problem, s_problem, sizeof(problem));
    strlcpy(ver, s_bversion, sizeof(ver));
    bool old = s_too_old;
    xSemaphoreGive(s_lock);
    for (char *c = ver; *c; c++) {
        if (*c == '"' || *c == '\\' || (unsigned char)*c < 0x20) {
            *c = '\'';
        }
    }
    for (char *c = problem; *c; c++) { // the bridge's own words in there: keep the JSON whole
        if (*c == '"' || *c == '\\' || (unsigned char)*c < 0x20) {
            *c = '\'';
        }
    }
    snprintf(out, len,
             "{\"state\":\"%s\",\"base\":\"%s\",\"name\":\"%s\",\"found_by\":\"%s\",\"override\":\"%s\","
             "\"picture\":\"%s\",\"udn\":\"%s\",\"version\":\"%s\",\"profile_problem\":\"%s\",\"update\":%s}",
             bridge_state_text(), base, s_name, s_found_by, s_override, bridge_picture(), s_udn, ver, problem,
             old ? "\"" BRIDGE_MIN_VERSION "\"" : "null");
}
