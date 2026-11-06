// Auto-rotate, see rotate.h.

#include "rotate.h"

#include <math.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs.h"

#include "board.h"

static const char *TAG = "rotate";

#define NVS_NS     "channelstv"
#define POLL_MS    200
#define STEADY     5     // polls the board has to stay one way (a second) before it turns
#define FLAT_G     0.75f // more than this along the sensor's z: lying flat, keep the rotation
#define MIN_G      0.5f  // the edge that is down needs at least this much of the pull ...
#define MARGIN_G   0.25f // ... and this much more than the other edge
#define AXES_DEFAULT 4 // its y axis points up the screen: checked on the board

static volatile int s_setting = ROTATE_AUTO;
static volatile int s_axes = AXES_DEFAULT;
static volatile int s_auto;
static volatile bool s_hold, s_fake, s_flat;
static float s_fake_v[3], s_a[3];

static void nvs_i32(const char *key, int32_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, key, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void want(void)
{
    board_rotation_want(s_hold ? 0 : s_setting >= 0 ? s_setting : s_auto);
}

// Which panel edge is down, as a rotation: the picture's bottom goes there. -1: none clearly.
static int down_edge(const float *a)
{
    // the sensor feels the push that holds it up: the pull is the other way
    float u = (s_axes & 1) ? a[1] : a[0], v = (s_axes & 1) ? a[0] : a[1];
    float gx = (s_axes & 2) ? u : -u, gy = (s_axes & 4) ? v : -v;
    s_flat = fabsf(a[2]) > FLAT_G;
    if (s_flat) {
        return -1;
    }
    float ax = fabsf(gx), ay = fabsf(gy);
    if (ay >= ax && ay > MIN_G && ay > ax + MARGIN_G) {
        return gy > 0 ? 0 : 2; // the bottom edge down: upright; the top edge down: upside down
    }
    if (ax > ay && ax > MIN_G && ax > ay + MARGIN_G) {
        return gx < 0 ? 1 : 3; // the left edge down: the top is at the right edge
    }
    return -1;
}

static void rotate_task(void *arg)
{
    int pending = -1, steady = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        float a[3];
        if (s_fake) {
            a[0] = s_fake_v[0];
            a[1] = s_fake_v[1];
            a[2] = s_fake_v[2];
        } else if (!board_accel(&a[0], &a[1], &a[2])) {
            continue;
        }
        s_a[0] = a[0];
        s_a[1] = a[1];
        s_a[2] = a[2];
        int e = down_edge(a);
        if (e < 0 || e != pending) {
            pending = e;
            steady = 0;
        } else if (++steady >= STEADY && s_auto != e) {
            s_auto = e;
            ESP_LOGI(TAG, "turned: %d", e);
        }
        want();
    }
}

void rotate_start(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        int32_t v;
        if (nvs_get_i32(h, "rotation", &v) == ESP_OK && v >= ROTATE_AUTO && v <= 3) {
            s_setting = v;
        }
        if (nvs_get_i32(h, "imu_axes", &v) == ESP_OK && v >= 0 && v <= 7) {
            s_axes = v;
        }
        nvs_close(h);
    }
    want();
    xTaskCreate(rotate_task, "rotate", 2560, NULL, 2, NULL);
}

int rotate_setting(void)
{
    return s_setting;
}

void rotate_set(int setting)
{
    s_setting = setting < ROTATE_AUTO || setting > 3 ? ROTATE_AUTO : setting;
    nvs_i32("rotation", s_setting);
    want();
}

int rotate_axes(void)
{
    return s_axes;
}

void rotate_set_axes(int map)
{
    s_axes = map & 7;
    nvs_i32("imu_axes", s_axes);
}

void rotate_hold(bool on)
{
    if (on != s_hold) {
        s_hold = on;
        want();
    }
}

void rotate_fake(bool fake, float x, float y, float z)
{
    s_fake_v[0] = x;
    s_fake_v[1] = y;
    s_fake_v[2] = z;
    s_fake = fake;
}

void rotate_status_json(char *out, size_t len)
{
    char setting[8];
    if (s_setting == ROTATE_AUTO) {
        snprintf(setting, sizeof(setting), "\"auto\"");
    } else {
        snprintf(setting, sizeof(setting), "%d", s_setting);
    }
    snprintf(out, len,
             "{\"setting\":%s,\"rotation\":%d,\"auto\":%d,\"axes\":%d,\"accel\":[%.2f,%.2f,%.2f],\"fake\":%s,"
             "\"flat\":%s,\"hold\":%s}",
             setting, board_rotation(), s_auto, s_axes, s_a[0], s_a[1], s_a[2], s_fake ? "true" : "false",
             s_flat ? "true" : "false", s_hold ? "true" : "false");
}
