// State shared by the screen, the keys and the web page.
#pragma once

#include <stdbool.h>
#include <stddef.h>

int app_volume(void);
void app_set_volume(int v); // 0..100, saved in NVS

typedef struct {
    bool connected;
    char ssid[33];
    char ip[16];
    int rssi;
} net_t;
void app_net(net_t *out);
bool app_online(void);

// Sets the Wi-Fi network and connects to it (the Wi-Fi driver keeps it in NVS).
void app_set_wifi(const char *ssid, const char *password);
// Forgets the network and restarts into the setup hotspot. The network is kept as the previous
// one, which the setup page offers to go back to.
void app_forget_wifi(void);
#if CONFIG_CHANNELSTV_WIFI_OUTAGE_TEST
// The test hook (Kconfig): the next start pretends the saved network is down for that long.
void app_fake_wifi_outage(int seconds);
#endif
// The network forgotten last; false if there is none. password may be NULL.
bool app_previous_wifi(char *ssid, size_t ssid_len, char *password, size_t pass_len);

// Updates over Wi-Fi (POST /api/ota) are taken only while this is on. Saved in NVS. Off on a
// fresh install; a board updated from a build without the setting keeps taking them. Set on
// the board only (Settings -> OTA updates): the network can't turn it on.
bool app_ota_enabled(void);
void app_set_ota_enabled(bool on);

// What the web page and API may do: WEB_FULL everything, WEB_READ_ONLY the status page and
// GET /api/status only (POST /api/ota keeps to its own switch), WEB_OFF no web server at all
// (but for the setup hotspot's page). Saved in NVS; read-only on a fresh install and on a board
// updated from a build without it. Loosened only with a finger on the board (Settings -> Web
// control), never from the network.
typedef enum { WEB_FULL, WEB_READ_ONLY, WEB_OFF } web_mode_t;
web_mode_t app_web_mode(void);
void app_set_web_mode(web_mode_t mode);

// Why the board last started: "power-on", "panic", "watchdog", "brownout", "software", ...
const char *app_reset_reason(void);

// Power off now (stops playback); on USB power, deep sleep until PWR is pressed.
void app_power_off(void);
