// Speaker playback, see player.h. One player task decodes what the pipe brings and writes it
// to the speaker; the pipe is a PSRAM stream buffer so the writer can run ahead.

#include "player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_aac_dec.h"
#include "esp_ae_rate_cvt.h"

#define PIPE_SIZE (256 * 1024)
#define CHUNK     512 // output samples per write

typedef struct {
    uint32_t gen;
    char what[64];
} cmd_t;

static esp_codec_dev_handle_t s_spk;
static int s_rate;
static player_amp_fn s_amp;
static QueueHandle_t s_cmds;
static volatile uint32_t s_gen;       // bumped by every play/stop; the running source checks it
static volatile bool s_playing;
static char s_what[64];
static char s_state[128] = "idle";
static volatile int64_t s_out_samples; // written to the speaker since this source started
static volatile uint32_t s_pipe_gen;   // the source accepting writes, 0 if none
static StreamBufferHandle_t s_pipe;
static volatile bool s_eof;

static void set_state(const char *fmt, const char *arg)
{
    snprintf(s_state, sizeof(s_state), fmt, arg);
}

// Mono PCM at s_rate to the speaker. Returns false once the source should stop.
static bool out_write(int16_t *pcm, size_t n, uint32_t gen)
{
    esp_codec_dev_write(s_spk, pcm, n * 2); // blocks at the playback rate
    s_out_samples += n;
    return gen == s_gen;
}

// <0 error, 0 end
static int pipe_read(uint8_t *buf, int len, uint32_t gen)
{
    int64_t deadline = esp_timer_get_time() + 15 * 1000000LL;
    while (gen == s_gen) {
        size_t n = xStreamBufferReceive(s_pipe, buf, len, pdMS_TO_TICKS(200));
        if (n > 0) {
            if (s_playing && s_state[0] == 'b') {
                set_state("playing %s", s_what); // after "buffering"
            }
            return n;
        }
        if (s_eof) {
            return 0;
        }
        if (esp_timer_get_time() > deadline) {
            return -1;
        }
        set_state("buffering %s", s_what);
    }
    return 0;
}

static const char *decode(uint32_t gen)
{
    esp_aac_dec_cfg_t aac = ESP_AAC_DEC_CONFIG_DEFAULT();
    esp_audio_simple_dec_cfg_t cfg = {.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_AAC, .dec_cfg = &aac, .cfg_size = sizeof(aac)};
    esp_audio_simple_dec_handle_t dec = NULL;
    if (esp_audio_simple_dec_open(&cfg, &dec) != ESP_AUDIO_ERR_OK) {
        return "no AAC decoder";
    }
    const char *err = NULL;
    uint32_t in_cap = 4096, out_cap = 16 * 1024, rs_cap = 0;
    uint8_t *in = heap_caps_malloc(in_cap, MALLOC_CAP_SPIRAM);
    uint8_t *out = heap_caps_malloc(out_cap, MALLOC_CAP_SPIRAM);
    int16_t *rs = NULL;
    esp_ae_rate_cvt_handle_t cvt = NULL;
    uint32_t cvt_rate = 0;
    bool eos = false;
    while (!eos && !err && gen == s_gen) {
        int n = pipe_read(in, in_cap, gen);
        if (n < 0) {
            err = "the sound stalled";
            break;
        }
        eos = n == 0;
        esp_audio_simple_dec_raw_t raw = {.buffer = in, .len = n, .eos = eos};
        while ((raw.len > 0 || eos) && !err && gen == s_gen) {
            esp_audio_simple_dec_out_t frame = {.buffer = out, .len = out_cap};
            esp_audio_err_t r = esp_audio_simple_dec_process(dec, &raw, &frame);
            if (r == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                out_cap = frame.needed_size;
                out = heap_caps_realloc(out, out_cap, MALLOC_CAP_SPIRAM);
                continue;
            }
            if (r != ESP_AUDIO_ERR_OK) {
                raw.consumed = raw.len; // skip a corrupt chunk instead of giving up on the stream
            }
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
            if (frame.decoded_size == 0) {
                if (raw.consumed == 0) {
                    break; // needs more input
                }
                continue;
            }
            esp_audio_simple_dec_info_t info;
            if (esp_audio_simple_dec_get_info(dec, &info) != ESP_AUDIO_ERR_OK || info.bits_per_sample != 16) {
                err = "unsupported sample format";
                break;
            }
            // mix to mono in place
            int16_t *pcm = (int16_t *)out;
            uint32_t samples = frame.decoded_size / 2 / info.channel;
            if (info.channel > 1) {
                for (uint32_t i = 0; i < samples; i++) {
                    int32_t sum = 0;
                    for (int c = 0; c < info.channel; c++) {
                        sum += pcm[i * info.channel + c];
                    }
                    pcm[i] = sum / info.channel;
                }
            }
            if (info.sample_rate != (uint32_t)s_rate) {
                if (!cvt || cvt_rate != info.sample_rate) {
                    if (cvt) {
                        esp_ae_rate_cvt_close(cvt);
                    }
                    esp_ae_rate_cvt_cfg_t rc = {.src_rate = info.sample_rate, .dest_rate = s_rate, .channel = 1,
                                                .bits_per_sample = 16, .complexity = 2,
                                                .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED};
                    if (esp_ae_rate_cvt_open(&rc, &cvt) != ESP_AE_ERR_OK) {
                        err = "unsupported sample rate";
                        break;
                    }
                    cvt_rate = info.sample_rate;
                }
                uint32_t need;
                esp_ae_rate_cvt_get_max_out_sample_num(cvt, samples, &need);
                if (need > rs_cap) {
                    rs_cap = need;
                    rs = heap_caps_realloc(rs, rs_cap * 2, MALLOC_CAP_SPIRAM);
                }
                uint32_t got = rs_cap;
                esp_ae_rate_cvt_process(cvt, pcm, samples, rs, &got);
                pcm = rs;
                samples = got;
            }
            if (!s_playing) {
                s_playing = true;
                set_state("playing %s", s_what);
            }
            for (uint32_t i = 0; i < samples; i += CHUNK) {
                uint32_t k = samples - i < CHUNK ? samples - i : CHUNK;
                if (!out_write(pcm + i, k, gen)) {
                    break;
                }
            }
            if (eos && raw.len == 0) {
                break;
            }
        }
    }
    if (cvt) {
        esp_ae_rate_cvt_close(cvt);
    }
    esp_audio_simple_dec_close(dec);
    free(in);
    free(out);
    free(rs);
    return err;
}

static void player_task(void *arg)
{
    static int16_t silence[CHUNK];
    cmd_t cmd;
    while (true) {
        xQueueReceive(s_cmds, &cmd, portMAX_DELAY);
        if (cmd.gen != s_gen) {
            continue; // superseded before it started
        }
        strlcpy(s_what, cmd.what, sizeof(s_what));
        s_out_samples = 0;
        xStreamBufferReset(s_pipe);
        s_eof = false;
        s_pipe_gen = cmd.gen;
        set_state("buffering %s", s_what);
        s_amp(true);
        while (cmd.gen == s_gen && !s_eof && xStreamBufferBytesAvailable(s_pipe) < 4096) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        const char *err = cmd.gen == s_gen ? decode(cmd.gen) : NULL;
        s_pipe_gen = 0;
        for (int i = 0; i < 4; i++) {
            esp_codec_dev_write(s_spk, silence, sizeof(silence)); // flush the DMA tail
        }
        s_amp(false);
        s_playing = false;
        if (err) {
            snprintf(s_state, sizeof(s_state), "error: %s", err);
        } else {
            strcpy(s_state, "idle");
        }
    }
}

void player_init(esp_codec_dev_handle_t spk, int sample_rate, player_amp_fn amp)
{
    s_spk = spk;
    s_rate = sample_rate;
    s_amp = amp;
    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();
    s_cmds = xQueueCreate(4, sizeof(cmd_t));
    s_pipe = xStreamBufferCreateWithCaps(PIPE_SIZE, 1, MALLOC_CAP_SPIRAM);
    // core 1, stack in PSRAM: the H.264 decoder has core 0
    xTaskCreatePinnedToCoreWithCaps(player_task, "player", 12288, NULL, 9, NULL, 1, MALLOC_CAP_SPIRAM);
}

uint32_t player_play_pipe(const char *name)
{
    cmd_t cmd = {0};
    strlcpy(cmd.what, name, sizeof(cmd.what));
    cmd.gen = ++s_gen; // stops whatever is playing now
    xQueueSend(s_cmds, &cmd, pdMS_TO_TICKS(100));
    return cmd.gen;
}

bool player_pipe_write_until(uint32_t gen, const void *data, size_t len, volatile bool *cancel)
{
    // wait for the player task to take the pipe; it resets the buffer first
    for (int i = 0; i < 200 && s_pipe_gen != gen && gen == s_gen && !(cancel && *cancel); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    const uint8_t *p = data;
    while (len && gen == s_gen && s_pipe_gen == gen && !(cancel && *cancel)) {
        size_t n = xStreamBufferSend(s_pipe, p, len, pdMS_TO_TICKS(100)); // blocks while full
        p += n;
        len -= n;
    }
    return len == 0;
}

void player_pipe_end(uint32_t gen)
{
    if (gen == s_gen) {
        s_eof = true;
    }
}

bool player_pipe_open(uint32_t gen)
{
    return s_pipe_gen == gen;
}

void player_stop(void)
{
    s_gen++;
}

uint32_t player_gen(void)
{
    return s_gen;
}

int64_t player_position_ms(void)
{
    // what's been written minus what still sits in the I2S DMA queue (8 x 480 frames)
    int64_t heard = s_out_samples - 8 * 480;
    return heard > 0 ? heard * 1000 / s_rate : 0;
}

bool player_is_playing(void)
{
    return s_playing;
}

const char *player_state(void)
{
    return s_state;
}
