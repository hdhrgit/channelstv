// The TV stream from the bridge, see stream.h.

#include "stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define RING (256 * 1024)       // ~5 s of the ~430 kbps stream; the bridge sends bursts ahead
#define CONNECT_TIMEOUT_MS 3000
#define READ_TIMEOUT_MS 1000    // every wait is this short, so a close is noticed quickly
#define FIRST_BYTE_MS 45000     // the DVR can take 15 s and more to start a channel, and the
                                // bridge tries it again after 20 s: one connection waits this long
#define STALL_MS 20000          // once data flowed: nothing this long on a connection, reconnect. The
                                // bridge passes the DVR's segments in bursts, with gaps of 9 and
                                // 14 s seen; a reconnect costs 15-30 s of tuning again
#define GIVE_UP_MS 60000        // no data at all for this long: fail
#define EDGE_MS 400             // catching up: a read that waits this long found the live edge
#define CATCH_UP_MAX_MS 60000   // and it stops after this long whatever happens
#define USER_AGENT "channelstv/" CHANNELSTV_VERSION

static const char *TAG = "stream";

static volatile uint32_t s_connects, s_reconnects, s_busy;
static volatile uint64_t s_bytes;
static volatile int s_first_byte_ms;
static char s_last_error[80];
static const char *volatile s_fstate = "idle";
static volatile int s_fetchers; // fetcher tasks still alive (a closed one needs up to ~1 s)
static volatile uint32_t s_fill;
static volatile bool s_drop_now; // test hook: the fetcher drops its connection as if it broke
static volatile uint32_t s_catchups, s_catch_bytes, s_catch_ms; // catch-ups, the latest one's bytes and time

// The fetcher owns this and frees it after the reader has closed it. The reader owns r and
// my_seg; the fetcher owns w, seg and seg_at. Both sides' 64-bit counters go through s_ring_mux.
struct stream {
    char url[320];
    bool live;                // endless: reconnect when it breaks; else it ends
    uint8_t *ring;
    uint64_t w, r;            // bytes written / read since opening
    uint32_t seg;             // connections after the first: each starts the stream over
    uint64_t seg_at;          // where the latest connection's bytes begin
    volatile uint32_t my_seg; // the connection the reader is in
    volatile bool quit, failed, busy, ended;
    volatile bool catch_up;   // asked by the reader, cleared by the fetcher when done
    uint32_t flush_seg;       // this connection's boundary drops the unread bytes before it
    int64_t roomy_at;         // the last time the ring was less than 3/4 full
    uint8_t *scratch;         // where caught-up bytes go
    char err[80];
    SemaphoreHandle_t data;   // given after each write
};

static portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;

static void src_fail(stream_t *s, const char *why)
{
    strlcpy(s->err, why, sizeof(s->err));
    strlcpy(s_last_error, why, sizeof(s_last_error));
    ESP_LOGW(TAG, "%s", why);
}

// Reads and drops what the bridge has waiting until a read has to wait for data (the live
// edge), then puts a boundary at the ring's end that the reader jumps to. -1: the connection
// broke (the boundary is left to the reconnect).
static int catch_up(stream_t *s, esp_http_client_handle_t c)
{
    s_fstate = "catching up";
    int64_t t0 = esp_timer_get_time();
    uint32_t got = 0;
    int how = 0;
    while (!s->quit && esp_timer_get_time() - t0 < CATCH_UP_MAX_MS * 1000LL) {
        int64_t a = esp_timer_get_time();
        int n = esp_http_client_read(c, (char *)s->scratch, 8192);
        int64_t took = esp_timer_get_time() - a;
        if (n > 0) {
            got += n;
            s_bytes += n;
        } else if (n != -ESP_ERR_HTTP_EAGAIN) {
            src_fail(s, n == 0 ? "stream ended, reconnecting" : "stream broke, reconnecting");
            how = -1;
            break;
        }
        if (n == -ESP_ERR_HTTP_EAGAIN || took > EDGE_MS * 1000LL) {
            break;
        }
    }
    s_catch_bytes = got;
    s_catch_ms = (esp_timer_get_time() - t0) / 1000;
    s_catchups++;
    s->catch_up = false;
    if (how < 0) {
        return how;
    }
    // the reader takes any earlier boundary first (a reconnect's: there is none in practice)
    s_fstate = "boundary";
    while (!s->quit && s->my_seg != s->seg) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    taskENTER_CRITICAL(&s_ring_mux);
    s->seg_at = s->w;
    s->seg++;
    s->flush_seg = s->seg;
    taskEXIT_CRITICAL(&s_ring_mux);
    xSemaphoreGive(s->data);
    ESP_LOGI(TAG, "caught up: %u bytes dropped in %u ms", (unsigned)got, (unsigned)s_catch_ms);
    return 0;
}

static void fetcher_task(void *arg)
{
    stream_t *s = arg;
    int64_t t_open = esp_timer_get_time(), last_data = t_open;
    bool ever = false; // any data from any connection yet
    int conn = 0;
    while (!s->quit && !s->failed && !s->ended) {
        esp_http_client_config_t cfg = {.url = s->url, .timeout_ms = CONNECT_TIMEOUT_MS, .buffer_size = 4096,
                                        .buffer_size_tx = 512, .user_agent = USER_AGENT};
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        int status = 0;
        int64_t t_conn = esp_timer_get_time();
        s_fstate = "connecting";
        if (c && esp_http_client_open(c, 0) == ESP_OK) {
            // the bridge may hold its headers back until the DVR sends: wait in short steps
            esp_http_client_set_timeout_ms(c, READ_TIMEOUT_MS);
            s_fstate = "waiting for the bridge";
            int64_t r;
            while ((r = esp_http_client_fetch_headers(c)) == -ESP_ERR_HTTP_EAGAIN && !s->quit &&
                   esp_timer_get_time() - t_conn < FIRST_BYTE_MS * 1000LL) {
            }
            status = r == -ESP_ERR_HTTP_EAGAIN || r == ESP_FAIL ? 0 : esp_http_client_get_status_code(c);
        }
        s_connects++;
        if (conn++ > 0) {
            s_reconnects++;
        }
        if (status == 200 && !s->quit) {
            s->busy = false;
            int64_t conn_data = esp_timer_get_time();
            bool started = false;
            while (!s->quit) {
                taskENTER_CRITICAL(&s_ring_mux);
                uint64_t w = s->w, used = s->w - s->r;
                taskEXIT_CRITICAL(&s_ring_mux);
                int room = RING - (int)used, contig = RING - (int)(w % RING);
                s_fill = used;
                if (used < RING * 3 / 4) {
                    s->roomy_at = esp_timer_get_time();
                }
                if (s->catch_up && started) {
                    int how = catch_up(s, c);
                    conn_data = last_data = esp_timer_get_time();
                    if (how < 0) {
                        break; // the connection broke meanwhile: reconnect as usual
                    }
                    continue;
                }
                // up to the ring's end at most; a short piece there is fine, a nearly full ring isn't
                int want = room < contig ? room : contig;
                want = want > 8192 ? 8192 : want;
                if (room < 2048) {
                    s_fstate = "ring full";
                    vTaskDelay(pdMS_TO_TICKS(20)); // the reader is behind: not a stall
                    conn_data = last_data = esp_timer_get_time();
                    continue;
                }
                if (s_drop_now) {
                    s_drop_now = false;
                    src_fail(s, "connection dropped (test), reconnecting");
                    break;
                }
                s_fstate = "reading";
                int n = esp_http_client_read(c, (char *)s->ring + w % RING, want);
                int64_t now = esp_timer_get_time();
                if (n > 0) {
                    if (!started) {
                        started = true;
                        if (!ever) {
                            s_first_byte_ms = (now - t_open) / 1000;
                        }
                        if (ever) {
                            // a new connection starts the stream over: wait for the reader to
                            // have taken the previous boundary, then mark this one
                            s_fstate = "boundary";
                            while (!s->quit && s->my_seg != s->seg) {
                                vTaskDelay(pdMS_TO_TICKS(20));
                            }
                            taskENTER_CRITICAL(&s_ring_mux);
                            s->seg_at = s->w;
                            s->seg++;
                            taskEXIT_CRITICAL(&s_ring_mux);
                            // the boundary moved: the bytes just read go after it, at the same w
                        }
                        ever = true;
                    }
                    taskENTER_CRITICAL(&s_ring_mux);
                    s->w += n;
                    taskEXIT_CRITICAL(&s_ring_mux);
                    s_bytes += n;
                    conn_data = last_data = now;
                    xSemaphoreGive(s->data);
                } else if (n == -ESP_ERR_HTTP_EAGAIN) {
                    s_fstate = started ? "reading" : "waiting for the first bytes";
                    if (!started && now - t_conn < FIRST_BYTE_MS * 1000LL) {
                        continue; // headers came, the stream itself hasn't started yet
                    }
                    if (now - conn_data > STALL_MS * 1000LL) {
                        src_fail(s, started ? "stream stalled, reconnecting" : "no data from the bridge, reconnecting");
                        break;
                    }
                } else if (!s->live && n == 0 && esp_http_client_is_complete_data_received(c)) {
                    s->ended = true; // a recording: it's over
                    break;
                } else {
                    src_fail(s, n == 0 ? "stream ended, reconnecting" : "stream broke, reconnecting");
                    if (!s->live) {
                        s->failed = true; // starting again would go back to the beginning
                    }
                    break;
                }
            }
        } else if (s->quit) {
            // closed while connecting: the cleanup below drops the connection
        } else if (status == 503) {
            s_busy++;
            s->busy = true;
            src_fail(s, "busy: the bridge already runs as many conversions as it allows");
            if (!ever) {
                s->failed = true;
            }
        } else {
            char why[64];
            snprintf(why, sizeof(why), status ? "HTTP %d from the bridge" : "no answer from the bridge", status);
            src_fail(s, why);
            if (!ever && status) {
                s->failed = true; // e.g. 404: no such channel
            }
        }
        if (c) {
            esp_http_client_cleanup(c); // closes the socket: the bridge sees the viewer leave
        }
        if (!s->failed && !s->quit && !s->ended) {
            int64_t quiet = esp_timer_get_time() - last_data;
            if (!ever && quiet > FIRST_BYTE_MS * 1000LL) {
                src_fail(s, "the channel didn't start (nothing in 45 s)");
                s->failed = true;
            } else if (quiet > GIVE_UP_MS * 1000LL) {
                src_fail(s, "no data from the bridge for 60 s");
                s->failed = true;
            }
        }
        s_fstate = "retry wait";
        for (int i = 0; i < (s->busy ? 30 : 5) && !s->quit && !s->failed && !s->ended; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    s_fstate = s->failed ? "failed" : s->ended ? "ended" : "closed";
    xSemaphoreGive(s->data); // wake a reader so it sees the failure
    while (!s->quit) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    heap_caps_free(s->ring);
    heap_caps_free(s->scratch);
    vSemaphoreDelete(s->data);
    heap_caps_free(s);
    taskENTER_CRITICAL(&s_ring_mux);
    s_fetchers--;
    taskEXIT_CRITICAL(&s_ring_mux);
    vTaskDeleteWithCaps(NULL);
}

stream_t *stream_open(const char *url, bool live)
{
    stream_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_SPIRAM);
    if (!s) {
        return NULL;
    }
    s_first_byte_ms = 0;
    s_fill = 0;
    s_drop_now = false;
    strlcpy(s->url, url, sizeof(s->url));
    s->live = live;
    s->ring = heap_caps_malloc(RING, MALLOC_CAP_SPIRAM);
    s->scratch = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    s->roomy_at = esp_timer_get_time();
    s->data = xSemaphoreCreateBinary();
    // internal RAM is tight: the last channel's connection has to be gone first
    for (int i = 0; i < 40 && s_fetchers > 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    taskENTER_CRITICAL(&s_ring_mux);
    s_fetchers++;
    taskEXIT_CRITICAL(&s_ring_mux);
    // stack in PSRAM: it only does network I/O. On core 1 with the demuxer, below the drawer
    // and the sound: core 0 is the H.264 decoder's.
    if (!s->ring || !s->scratch || !s->data ||
        xTaskCreatePinnedToCoreWithCaps(fetcher_task, "livefetch", 6144, s, 6, NULL, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
        taskENTER_CRITICAL(&s_ring_mux);
        s_fetchers--;
        taskEXIT_CRITICAL(&s_ring_mux);
        heap_caps_free(s->ring);
        heap_caps_free(s->scratch);
        if (s->data) {
            vSemaphoreDelete(s->data);
        }
        heap_caps_free(s);
        return NULL;
    }
    return s;
}

int stream_read(stream_t *s, uint8_t pkt[188], int timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + timeout_ms * 1000LL;
    while (true) {
        taskENTER_CRITICAL(&s_ring_mux);
        uint64_t w = s->w, r = s->r, at = s->seg_at;
        uint32_t seg = s->seg;
        taskEXIT_CRITICAL(&s_ring_mux);
        bool boundary = s->my_seg != seg;
        if (boundary && s->flush_seg == seg) {
            taskENTER_CRITICAL(&s_ring_mux);
            s->r = at; // caught up: what was waiting in the ring goes
            taskEXIT_CRITICAL(&s_ring_mux);
            s->my_seg = seg;
            return STREAM_CAUGHT_UP;
        }
        uint64_t limit = boundary ? at : w;
        if (boundary && limit - r < 188) {
            taskENTER_CRITICAL(&s_ring_mux);
            s->r = at; // a torn last packet of the old connection goes too
            taskEXIT_CRITICAL(&s_ring_mux);
            s->my_seg = seg;
            return STREAM_RESTART;
        }
        if (limit - r >= 188) {
            if (s->ring[r % RING] != 0x47) {
                taskENTER_CRITICAL(&s_ring_mux);
                s->r = r + 1; // lost sync: find the next packet start
                taskEXIT_CRITICAL(&s_ring_mux);
                continue;
            }
            int off = r % RING, first = RING - off < 188 ? RING - off : 188;
            memcpy(pkt, s->ring + off, first);
            memcpy(pkt + first, s->ring, 188 - first);
            taskENTER_CRITICAL(&s_ring_mux);
            s->r = r + 188;
            taskEXIT_CRITICAL(&s_ring_mux);
            return STREAM_PACKET;
        }
        if (s->failed) {
            return STREAM_FAILED;
        }
        if (s->ended && !boundary) {
            return STREAM_END;
        }
        int64_t left = deadline - esp_timer_get_time();
        if (left <= 0) {
            return STREAM_TIMEOUT;
        }
        xSemaphoreTake(s->data, pdMS_TO_TICKS(left / 1000 < 100 ? left / 1000 + 1 : 100));
    }
}

void stream_catch_up(stream_t *s)
{
    s->catch_up = true;
}

bool stream_catching_up(stream_t *s)
{
    return s->catch_up;
}

uint32_t stream_fill(stream_t *s)
{
    taskENTER_CRITICAL(&s_ring_mux);
    uint32_t used = s->w - s->r;
    taskEXIT_CRITICAL(&s_ring_mux);
    return used;
}

int stream_full_ms(stream_t *s)
{
    return stream_fill(s) < RING * 3 / 4 ? 0 : (esp_timer_get_time() - s->roomy_at) / 1000;
}

uint32_t stream_caught_up_bytes(void)
{
    return s_catch_bytes;
}

void stream_test_drop(void)
{
    s_drop_now = true;
}

const char *stream_error(stream_t *s)
{
    return s->err;
}

bool stream_busy(stream_t *s)
{
    return s->busy;
}

void stream_close(stream_t *s)
{
    if (s) {
        s->quit = true;
    }
}

void stream_status_json(char *out, size_t len)
{
    snprintf(out, len,
             "{\"connects\":%u,\"reconnects\":%u,\"busy\":%u,\"bytes\":%llu,\"first_byte_ms\":%d,\"state\":\"%s\","
             "\"ring\":%u,\"catchups\":%u,\"catchup_bytes\":%u,\"catchup_ms\":%u,\"last_error\":\"%s\"}",
             (unsigned)s_connects, (unsigned)s_reconnects, (unsigned)s_busy, (unsigned long long)s_bytes, s_first_byte_ms,
             s_fstate, (unsigned)s_fill, (unsigned)s_catchups, (unsigned)s_catch_bytes, (unsigned)s_catch_ms, s_last_error);
}
