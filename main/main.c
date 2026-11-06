// channelstv: live TV and recordings from a Channels DVR, through its DLNA bridge, on the
// Waveshare ESP32-S3-Touch-LCD-1.54.
//
// Screen: the bridge's folders (Live TV, recordings, ...); tap to open or play, tap the
// picture to stop. Keys: PWR stops playback or goes back a folder (hold it to power off on
// battery), VOL+/VOL- change the volume.
// Web: http://<board-ip>/ shows the status and sets the bridge address (see web.h).
// Wi-Fi: the network the Wi-Fi driver last saved, else the one from menuconfig, else the setup
// hotspot (setup.h); 'W' ssid TAB password over the USB console sets another one.
// Update over Wi-Fi: POST build/channelstv.bin to /api/ota; a new image that can't get onto
// Wi-Fi within 90 s is rolled back.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "app.h"
#include "board.h"
#include "bridge.h"
#include "player.h"
#include "rotate.h"
#include "setup.h"
#include "tv.h"
#include "ui.h"
#include "web.h"

static const char *TAG = "channelstv";

static int s_volume = 60;
static volatile bool s_connected;
static char s_ip[16] = "";

// ---------------------------------------------------------------- volume

int app_volume(void)
{
    return s_volume;
}

void app_set_volume(int v)
{
    s_volume = v < 0 ? 0 : v > 100 ? 100 : v;
    if (board_speaker()) {
        esp_codec_dev_set_out_vol(board_speaker(), s_volume);
    }
    nvs_handle_t h;
    if (nvs_open("channelstv", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "volume", s_volume);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void volume_load(void)
{
    nvs_handle_t h;
    if (nvs_open("channelstv", NVS_READONLY, &h) == ESP_OK) {
        int32_t v;
        if (nvs_get_i32(h, "volume", &v) == ESP_OK) {
            s_volume = v;
        }
        nvs_close(h);
    }
}

const char *app_reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "reset pin";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB: return "USB";
    default: return "unknown";
    }
}

// ---------------------------------------------------------------- power

// Idle, the board runs the CPU at 80 MHz and keeps Wi-Fi in modem sleep. Playing needs the
// full 240 MHz and an always-on radio: streams stutter in power save. power_task flips
// between the two with 2 s of grace.
static esp_pm_lock_handle_t s_busy_lock;

static void power_task(void *arg)
{
    bool busy = false;
    int hold = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(250));
        if (tv_playing() || player_is_playing()) {
            hold = 8;
        } else if (hold > 0) {
            hold--;
        }
        if ((hold > 0) != busy) {
            busy = hold > 0;
            if (busy) {
                esp_pm_lock_acquire(s_busy_lock);
                esp_wifi_set_ps(WIFI_PS_NONE);
            } else {
                esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
                esp_pm_lock_release(s_busy_lock);
            }
        }
    }
}

void app_power_off(void)
{
    tv_stop();
    player_stop();
    vTaskDelay(pdMS_TO_TICKS(300));
    board_power_off(0);
}

// ---------------------------------------------------------------- keys

static void on_key(board_key_t key, press_t press)
{
    ui_wake();
    switch (key) {
    case KEY_POWER:
        if (press == PRESS_HOLD) {
            ui_power_hold(board_power_hold_ms());
        } else if (press == PRESS_CANCEL) {
            ui_power_hold(0);
        } else if (press == PRESS_LONG) {
            tv_stop();
            vTaskDelay(pdMS_TO_TICKS(100));
            ui_power_hold(board_power_hold_ms()); // "Powering off": full ring, then dark
            vTaskDelay(pdMS_TO_TICKS(600));
            app_power_off(); // only cuts the power on battery
            ui_power_hold(0); // still here: on USB power the latch can't cut it
        } else if (tv_playing()) {
            tv_stop();
        } else {
            ui_back();
        }
        break;
    case KEY_VOL_UP:
        app_set_volume(app_volume() + 5);
        ui_show_volume(app_volume());
        break;
    case KEY_VOL_DOWN:
        app_set_volume(app_volume() - 5);
        ui_show_volume(app_volume());
        break;
    }
}

// ---------------------------------------------------------------- wi-fi

static bool sta_saved(void)
{
    wifi_config_t cfg = {0};
    return esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK && cfg.sta.ssid[0];
}

static esp_timer_handle_t s_retry;
// The network saved when the setup hotspot opened (none on the first start): it goes back in
// place of a network picked on the setup page that doesn't take, so a wrong password neither
// stands in for a network that works nor keeps the hotspot shut at the next start.
static wifi_config_t s_before;

static void open_setup(void)
{
    memset(&s_before, 0, sizeof(s_before));
    esp_wifi_get_config(WIFI_IF_STA, &s_before);
    setup_start(s_before.sta.ssid[0] ? (const char *)s_before.sta.ssid : NULL);
}

static void retry(void *arg)
{
    esp_wifi_connect();
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (sta_saved()) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        s_ip[0] = '\0';
        if (setup_active()) {
            // the hotspot needs the radio for scans: retry by a timer, and give up on a network
            // picked on the setup page that doesn't take (the page says why)
            int ms = setup_on_disconnected(((wifi_event_sta_disconnected_t *)data)->reason);
            if (ms >= 0) {
                esp_timer_stop(s_retry);
                esp_timer_start_once(s_retry, ms * 1000LL);
            } else {
                esp_wifi_set_config(WIFI_IF_STA, &s_before);
            }
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        ESP_LOGI(TAG, "got ip %s", s_ip);
        // reaching the network proves a freshly flashed image is good
        esp_ota_mark_app_valid_cancel_rollback();
        setup_on_got_ip(s_ip);
    }
}

static void wifi_start(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(netif, "channelstv");
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&init_cfg);
    const esp_timer_create_args_t rt = {.callback = retry, .name = "wifi retry"};
    esp_timer_create(&rt, &s_retry);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);
    esp_wifi_set_mode(WIFI_MODE_STA);
    wifi_config_t cfg = {0};
    esp_wifi_get_config(WIFI_IF_STA, &cfg);
    if (cfg.sta.ssid[0] == '\0' && CONFIG_CHANNELSTV_WIFI_SSID[0]) {
        // nothing saved yet: the network from menuconfig
        strlcpy((char *)cfg.sta.ssid, CONFIG_CHANNELSTV_WIFI_SSID, sizeof(cfg.sta.ssid));
        strlcpy((char *)cfg.sta.password, CONFIG_CHANNELSTV_WIFI_PASSWORD, sizeof(cfg.sta.password));
        esp_wifi_set_config(WIFI_IF_STA, &cfg);
    }
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM); // power_task switches it off while something plays
    if (!cfg.sta.ssid[0]) {
        ESP_LOGI(TAG, "no network saved: setup hotspot");
        open_setup();
    }
}

void app_net(net_t *out)
{
    memset(out, 0, sizeof(*out));
    out->connected = s_connected;
    strlcpy(out->ip, s_ip, sizeof(out->ip));
    wifi_ap_record_t ap;
    if (s_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        strlcpy(out->ssid, (const char *)ap.ssid, sizeof(out->ssid));
        out->rssi = ap.rssi;
    } else {
        wifi_config_t cfg;
        if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK) {
            strlcpy(out->ssid, (const char *)cfg.sta.ssid, sizeof(out->ssid));
        }
    }
}

bool app_online(void)
{
    return s_connected;
}

void app_set_wifi(const char *ssid, const char *password)
{
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password));
    setup_on_connecting(ssid);
    esp_timer_stop(s_retry);
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &cfg); // persisted to NVS by the Wi-Fi driver
    esp_wifi_connect();
}

// The network forgotten last, kept in our NVS namespace so the setup page can go back to it.
bool app_previous_wifi(char *ssid, size_t ssid_len, char *password, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open("channelstv", NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    char s[33] = "", p[65] = "";
    size_t sl = sizeof(s), pl = sizeof(p);
    bool ok = nvs_get_str(h, "prev_ssid", s, &sl) == ESP_OK && s[0];
    if (ok && nvs_get_str(h, "prev_pass", p, &pl) != ESP_OK) {
        p[0] = '\0';
    }
    nvs_close(h);
    if (ok) {
        strlcpy(ssid, s, ssid_len);
        if (password) {
            strlcpy(password, p, pass_len);
        }
    }
    return ok;
}

void app_forget_wifi(void)
{
    wifi_config_t cfg = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK && cfg.sta.ssid[0]) {
        nvs_handle_t h;
        if (nvs_open("channelstv", NVS_READWRITE, &h) == ESP_OK) {
            char s[33], p[65];
            strlcpy(s, (const char *)cfg.sta.ssid, sizeof(s));
            strlcpy(p, (const char *)cfg.sta.password, sizeof(p));
            nvs_set_str(h, "prev_ssid", s);
            nvs_set_str(h, "prev_pass", p);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    ESP_LOGI(TAG, "forgetting the Wi-Fi network");
    tv_stop();
    memset(&cfg, 0, sizeof(cfg));
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart(); // starts as on the first boot: the setup hotspot
}

// 'W' ssid TAB password, typed or piped into the USB console
static void console_task(void *arg)
{
    char line[160];
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *tab = strchr(line, '\t');
        if (line[0] != 'W' || !tab) {
            continue;
        }
        *tab = '\0';
        app_set_wifi(line + 1, tab + 1);
        printf("wifi set to %s\n", line + 1);
    }
    vTaskDelete(NULL);
}

// A saved network that isn't there two minutes after start (moved house, new router password):
// the setup hotspot opens next to it, and the board keeps trying the saved one.
static void setup_fallback(void *arg)
{
    wifi_config_t cfg = {0};
    if (!s_connected && esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK && cfg.sta.ssid[0]) {
        ESP_LOGW(TAG, "%s not reachable: setup hotspot", (const char *)cfg.sta.ssid);
        open_setup();
    }
}

// An updated image that never reaches the network reboots, and the bootloader rolls back.
static void rollback_watchdog(void *arg)
{
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGE(TAG, "no network after the update, rolling back");
        esp_restart();
    }
}

void app_main(void)
{
    board_init(); // first: holds the battery latch on
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    volume_load();
    ui_start();
    rotate_start();
    tv_init();
    if (board_speaker()) {
        esp_codec_dev_set_out_vol(board_speaker(), s_volume);
        player_init(board_speaker(), SAMPLE_RATE, board_amp);
    }
    board_keys_start(on_key);
    wifi_start();
    bridge_start(app_online);
    web_start();
    xTaskCreate(console_task, "console", 3072, NULL, 3, NULL);
    esp_pm_config_t pm = {.max_freq_mhz = 240, .min_freq_mhz = 80, .light_sleep_enable = false};
    esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "busy", &s_busy_lock);
    esp_pm_configure(&pm);
    xTaskCreate(power_task, "power", 2560, NULL, 3, NULL);
    const esp_timer_create_args_t wd = {.callback = rollback_watchdog, .name = "rollback"};
    esp_timer_handle_t t;
    esp_timer_create(&wd, &t);
    esp_timer_start_once(t, 90 * 1000000LL);
    const esp_timer_create_args_t fb = {.callback = setup_fallback, .name = "setup fallback"};
    esp_timer_create(&fb, &t);
    esp_timer_start_once(t, 120 * 1000000LL);
}
