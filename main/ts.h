// MPEG-TS demuxer: PAT, PMT, PES reassembly for one H.264 and one AAC (ADTS) stream, and the
// picture size from an H.264 SPS.
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum { TS_VIDEO, TS_AUDIO };
// A whole PES payload. pts is the 33-bit PTS, -1 if it had none. gap: packets of this
// stream were lost (continuity counter) since the last PES that came out.
typedef void (*ts_pes_fn)(void *ctx, int kind, int64_t pts, uint8_t *data, int len, bool gap);
typedef struct {
    ts_pes_fn on_pes;
    void *ctx;
    int pmt_pid, vpid, apid; // 0 until known
    uint8_t *buf[2];         // video, audio
    int cap[2], len[2], want[2]; // want: the PES length if it gave one, else 0 (ends at the next start)
    int64_t pts[2];
    bool open[2], gap[2];
    int cc[2];
    // a reserved header in front of each PES's data (the player keeps its own fields there)
    int head;
    uint32_t packets, cc_errors, bad_sync, dropped_pes;
} ts_demux_t;

bool ts_init(ts_demux_t *d, int video_cap, int audio_cap, int head, ts_pes_fn fn, void *ctx);
void ts_free(ts_demux_t *d);
// Forget all streams' state (after a new connection): the PAT and PMT are read again.
void ts_reset(ts_demux_t *d);
void ts_push(ts_demux_t *d, const uint8_t pkt[188]);

// The picture size an H.264 access unit's SPS gives (after cropping); false if it has no SPS.
bool h264_sps_size(const uint8_t *au, int len, int *w, int *h);
