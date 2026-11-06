// The speaker: the sound of the TV stream, AAC in ADTS frames, fed in through a pipe by the
// TV player. It is decoded, mixed to mono and resampled to the I2S rate before it reaches
// the ES8311.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_codec_dev.h"

typedef void (*player_amp_fn)(bool on);

void player_init(esp_codec_dev_handle_t spk, int sample_rate, player_amp_fn amp);

// A new source: AAC bytes go in with player_pipe_write_until(), which blocks while the buffer
// is full and gives up (false) once this source has been stopped or *cancel turns true.
uint32_t player_play_pipe(const char *name);
bool player_pipe_write_until(uint32_t gen, const void *data, size_t len, volatile bool *cancel);
void player_pipe_end(uint32_t gen);
// The player has taken the pipe and reset its position: the clock below now belongs to it.
bool player_pipe_open(uint32_t gen);
void player_stop(void);
// Changes with every play or stop.
uint32_t player_gen(void);
// How much of the current source has come out of the speaker, for keeping the picture in step.
int64_t player_position_ms(void);
bool player_is_playing(void);
// "idle", "buffering ...", "playing ...", "error: ..."
const char *player_state(void);
