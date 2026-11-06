// Wi-Fi setup hotspot and its DNS server, see setup.h.

#include "setup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "cJSON.h"

#include "web.h"

static const char *TAG = "setup";

#define TRIES      3        // failed joins before giving up on a network ...
#define RETRY_MS   30000    // ... or, for the saved one, before trying only this often
#define CLOSE_US   30000000 // the hotspot stays this long after joining, for the page to show the address

static esp_netif_t *s_ap;
static volatile bool s_active;
static volatile unsigned s_gen;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_status[96];
static char s_ssid[33];
static int s_fails;
static bool s_picked; // s_ssid came from the setup page, not from the saved settings
static bool s_failed;
static TaskHandle_t s_dns;
static esp_timer_handle_t s_close;

static void set_status(const char *text, bool failed)
{
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_status, text, sizeof(s_status));
    s_failed = failed;
    s_gen++;
    taskEXIT_CRITICAL(&s_lock);
    if (text[0]) {
        ESP_LOGI(TAG, "%s", text);
    }
}

void setup_status(char *out, size_t len)
{
    taskENTER_CRITICAL(&s_lock);
    strlcpy(out, s_status, len);
    taskEXIT_CRITICAL(&s_lock);
}

bool setup_failed(void)
{
    return s_failed;
}

bool setup_active(void)
{
    return s_active;
}

unsigned setup_gen(void)
{
    return s_gen;
}

void setup_ap_ip(char *out, size_t len)
{
    esp_netif_ip_info_t ip;
    if (s_active && s_ap && esp_netif_get_ip_info(s_ap, &ip) == ESP_OK) {
        snprintf(out, len, IPSTR, IP2STR(&ip.ip));
    } else if (len) {
        out[0] = '\0';
    }
}

// Every A question gets the hotspot's address; anything else an empty answer.
static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    struct timeval tv = {.tv_sec = 1};
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "dns: no socket");
        if (sock >= 0) {
            close(sock);
        }
        s_dns = NULL;
        vTaskDelete(NULL);
    }
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t buf[512];
    while (s_active) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &flen);
        if (n < 12 || (buf[2] & 0x80) || buf[4] != 0 || buf[5] != 1) {
            continue; // not a query with one question
        }
        // the question's name, then type and class
        int q = 12;
        while (q < n && buf[q]) {
            q += buf[q] + 1;
        }
        if (q + 5 > n) {
            continue;
        }
        bool a = buf[q + 1] == 0 && buf[q + 2] == 1;
        int end = q + 5;
        buf[2] = 0x84 | (buf[2] & 0x01); // a response, authoritative, RD copied
        buf[3] = 0x80;                   // RA, no error
        buf[6] = 0;
        buf[7] = a ? 1 : 0;
        memset(buf + 8, 0, 4); // no authority or additional records
        if (a) {
            esp_netif_ip_info_t ip = {0};
            esp_netif_get_ip_info(s_ap, &ip);
            const uint8_t rr[12] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4}; // the name above, A, IN, 60 s
            memcpy(buf + end, rr, sizeof(rr));
            memcpy(buf + end + 12, &ip.ip.addr, 4);
            end += 16;
        }
        sendto(sock, buf, end, 0, (struct sockaddr *)&from, flen);
    }
    close(sock);
    s_dns = NULL;
    vTaskDelete(NULL);
}

static void close_hotspot(void *arg)
{
    if (!s_active) {
        return;
    }
    ESP_LOGI(TAG, "closing the hotspot");
    s_active = false; // the DNS task ends within a second
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    set_status("", false);
    web_apply(); // with Web control off, the server goes with the hotspot
}

void setup_start(const char *saved)
{
    if (s_active) {
        return;
    }
    if (!s_ap) {
        s_ap = esp_netif_create_default_wifi_ap();
        const esp_timer_create_args_t t = {.callback = close_hotspot, .name = "setup"};
        esp_timer_create(&t, &s_close);
    }
    wifi_config_t ap = {
        .ap = {
            .ssid = SETUP_SSID,
            .ssid_len = sizeof(SETUP_SSID) - 1,
            .channel = 1,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 4,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_wifi_set_ps(WIFI_PS_NONE); // phones on the hotspot miss beacons otherwise
    strlcpy(s_ssid, saved ? saved : "", sizeof(s_ssid));
    s_picked = false;
    s_fails = 0;
    s_active = true;
    set_status("", false);
    ESP_LOGI(TAG, "hotspot %s open", SETUP_SSID);
    for (int i = 0; s_dns && i < 20; i++) {
        vTaskDelay(pdMS_TO_TICKS(100)); // the last one still winding down
    }
    if (!s_dns) {
        xTaskCreate(dns_task, "dns", 3072, NULL, 3, &s_dns);
    }
}

void setup_on_connecting(const char *ssid)
{
    if (!s_active) {
        return;
    }
    esp_timer_stop(s_close);
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    s_picked = true;
    s_fails = 0;
    char text[96];
    snprintf(text, sizeof(text), "Connecting to %s...", ssid);
    set_status(text, false);
}

int setup_on_disconnected(int reason)
{
    if (!s_ssid[0]) {
        return -1; // nothing picked yet
    }
    if (++s_fails < TRIES) {
        return 1000;
    }
    if (s_fails > TRIES) {
        return RETRY_MS; // the status already says why
    }
    char text[96];
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        snprintf(text, sizeof(text), "%s not found", s_ssid);
        break;
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_MIC_FAILURE:
        snprintf(text, sizeof(text), "Wrong password for %s?", s_ssid);
        break;
    default:
        snprintf(text, sizeof(text), "Couldn't join %s (reason %d)", s_ssid, reason);
        break;
    }
    set_status(text, true);
    return s_picked ? -1 : RETRY_MS;
}

void setup_on_got_ip(const char *ip)
{
    if (!s_active) {
        return;
    }
    char text[96];
    snprintf(text, sizeof(text), "Connected to %s: http://%s/", s_ssid, ip);
    set_status(text, false);
    esp_timer_stop(s_close);
    esp_timer_start_once(s_close, CLOSE_US);
}

char *setup_scan_json(void)
{
    wifi_scan_config_t sc = {.show_hidden = false};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        return NULL;
    }
    uint16_t n = 24;
    wifi_ap_record_t *rec = calloc(n, sizeof(*rec));
    if (!rec) {
        esp_wifi_clear_ap_list();
        return NULL;
    }
    esp_wifi_scan_get_ap_records(&n, rec); // strongest first
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        const char *ssid = (const char *)rec[i].ssid;
        bool seen = !ssid[0] || !strcmp(ssid, SETUP_SSID);
        for (int k = 0; k < i && !seen; k++) {
            seen = !strcmp(ssid, (const char *)rec[k].ssid);
        }
        if (seen) {
            continue;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", ssid);
        cJSON_AddNumberToObject(o, "rssi", rec[i].rssi);
        cJSON_AddBoolToObject(o, "secure", rec[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, o);
    }
    free(rec);
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return s;
}
