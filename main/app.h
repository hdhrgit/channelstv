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

// Why the board last started: "power-on", "panic", "watchdog", "brownout", "software", ...
const char *app_reset_reason(void);

// Power off now (stops playback); on USB power, deep sleep until PWR is pressed.
void app_power_off(void);
