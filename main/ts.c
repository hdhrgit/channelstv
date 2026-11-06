// MPEG-TS demuxer, see ts.h.

#include "ts.h"

#include <string.h>
#include "esp_heap_caps.h"

bool ts_init(ts_demux_t *d, int video_cap, int audio_cap, int head, ts_pes_fn fn, void *ctx)
{
    memset(d, 0, sizeof(*d));
    d->on_pes = fn;
    d->ctx = ctx;
    d->head = head;
    d->cap[TS_VIDEO] = video_cap;
    d->cap[TS_AUDIO] = audio_cap;
    for (int k = 0; k < 2; k++) {
        d->buf[k] = heap_caps_malloc(head + d->cap[k], MALLOC_CAP_SPIRAM);
    }
    ts_reset(d);
    return d->buf[0] && d->buf[1];
}

void ts_free(ts_demux_t *d)
{
    heap_caps_free(d->buf[0]);
    heap_caps_free(d->buf[1]);
    d->buf[0] = d->buf[1] = NULL;
}

void ts_reset(ts_demux_t *d)
{
    d->pmt_pid = d->vpid = d->apid = 0;
    for (int k = 0; k < 2; k++) {
        d->open[k] = d->gap[k] = false;
        d->len[k] = d->want[k] = 0;
        d->cc[k] = -1;
    }
}

static void emit(ts_demux_t *d, int k)
{
    d->open[k] = false;
    d->on_pes(d->ctx, k, d->pts[k], d->buf[k] + d->head, d->len[k], d->gap[k]);
    d->gap[k] = false;
}

static void lose(ts_demux_t *d, int k)
{
    if (d->open[k]) {
        d->dropped_pes++;
        d->open[k] = false;
    }
    d->gap[k] = true;
}

// A PSI section's start (after the pointer field) in a packet's payload, NULL if it doesn't fit.
static const uint8_t *section(const uint8_t *p, int plen, int *slen)
{
    if (plen < 1 || 1 + p[0] + 3 > plen) {
        return NULL;
    }
    const uint8_t *s = p + 1 + p[0];
    *slen = (s[1] & 0x0f) << 8 | s[2];
    if (s + 3 + *slen > p + plen) {
        *slen = p + plen - s - 3; // the rest is in the next packet: what fits is enough here
    }
    return s;
}

static void parse_pat(ts_demux_t *d, const uint8_t *p, int plen)
{
    int slen;
    const uint8_t *s = section(p, plen, &slen);
    if (!s || s[0] != 0x00) {
        return;
    }
    for (int i = 8; i + 4 <= 3 + slen - 4; i += 4) { // programs, then the CRC
        int prog = s[i] << 8 | s[i + 1], pid = (s[i + 2] & 0x1f) << 8 | s[i + 3];
        if (prog != 0) {
            d->pmt_pid = pid;
            return;
        }
    }
}

static void parse_pmt(ts_demux_t *d, const uint8_t *p, int plen)
{
    int slen;
    const uint8_t *s = section(p, plen, &slen);
    if (!s || s[0] != 0x02 || slen < 13) {
        return;
    }
    int vpid = 0, apid = 0;
    int i = 12 + ((s[10] & 0x0f) << 8 | s[11]);
    while (i + 5 <= 3 + slen - 4) {
        int type = s[i], pid = (s[i + 1] & 0x1f) << 8 | s[i + 2], info = (s[i + 3] & 0x0f) << 8 | s[i + 4];
        if (type == 0x1b && !vpid) {
            vpid = pid; // H.264
        } else if (type == 0x0f && !apid) {
            apid = pid; // AAC in ADTS
        }
        i += 5 + info;
    }
    if (vpid != d->vpid || apid != d->apid) {
        d->vpid = vpid;
        d->apid = apid;
        for (int k = 0; k < 2; k++) {
            d->open[k] = false;
            d->cc[k] = -1;
        }
    }
}

void ts_push(ts_demux_t *d, const uint8_t pkt[188])
{
    if (pkt[0] != 0x47) {
        d->bad_sync++;
        return;
    }
    d->packets++;
    if (pkt[1] & 0x80) {
        return; // transport error flag
    }
    bool pusi = pkt[1] & 0x40;
    int pid = (pkt[1] & 0x1f) << 8 | pkt[2], afc = pkt[3] >> 4 & 3, cc = pkt[3] & 0x0f;
    if (!(afc & 1)) {
        return; // adaptation field only: no payload, and the counter doesn't move
    }
    int off = 4;
    if (afc & 2) {
        off += 1 + pkt[4];
    }
    if (off >= 188) {
        return;
    }
    const uint8_t *p = pkt + off;
    int plen = 188 - off;
    if (pid == 0) {
        if (pusi) {
            parse_pat(d, p, plen);
        }
        return;
    }
    if (pid == d->pmt_pid) {
        if (pusi) {
            parse_pmt(d, p, plen);
        }
        return;
    }
    int k = pid == d->vpid && d->vpid ? TS_VIDEO : pid == d->apid && d->apid ? TS_AUDIO : -1;
    if (k < 0) {
        return;
    }
    if (d->cc[k] >= 0) {
        if (cc == d->cc[k]) {
            return; // a repeated packet
        }
        if (cc != ((d->cc[k] + 1) & 0x0f)) {
            d->cc_errors++;
            lose(d, k);
        }
    }
    d->cc[k] = cc;
    if (pusi) {
        if (d->open[k]) {
            emit(d, k); // an unbounded (video) PES ends where the next one starts
        }
        if (plen < 9 || p[0] || p[1] || p[2] != 1 || 9 + p[8] > plen) {
            lose(d, k);
            return;
        }
        int pes_len = p[4] << 8 | p[5], hl = p[8];
        d->pts[k] = -1;
        if ((p[7] & 0x80) && hl >= 5) {
            d->pts[k] = (int64_t)(p[9] >> 1 & 7) << 30 | (int64_t)p[10] << 22 | (int64_t)(p[11] >> 1) << 15 |
                        (int64_t)p[12] << 7 | p[13] >> 1;
        }
        d->want[k] = pes_len ? pes_len - 3 - hl : 0;
        d->len[k] = 0;
        d->open[k] = true;
        p += 9 + hl;
        plen -= 9 + hl;
    } else if (!d->open[k]) {
        return; // the middle of a PES whose start we never saw
    }
    if (d->len[k] + plen > d->cap[k]) {
        lose(d, k);
        return;
    }
    memcpy(d->buf[k] + d->head + d->len[k], p, plen);
    d->len[k] += plen;
    if (d->want[k] > 0 && d->len[k] >= d->want[k]) {
        d->len[k] = d->want[k];
        emit(d, k);
    }
}

// ---------------------------------------------------------------- SPS

typedef struct {
    const uint8_t *b;
    int n, bit;
} bits_t;

static int get_bit(bits_t *r)
{
    if (r->bit >= r->n * 8) {
        return 0;
    }
    int v = r->b[r->bit >> 3] >> (7 - (r->bit & 7)) & 1;
    r->bit++;
    return v;
}

static unsigned get_bits(bits_t *r, int n)
{
    unsigned v = 0;
    while (n--) {
        v = v << 1 | get_bit(r);
    }
    return v;
}

static unsigned get_ue(bits_t *r)
{
    int zeros = 0;
    while (!get_bit(r) && zeros < 31 && r->bit < r->n * 8) {
        zeros++;
    }
    return (1u << zeros) - 1 + get_bits(r, zeros);
}

static int get_se(bits_t *r)
{
    unsigned v = get_ue(r);
    return v & 1 ? (int)(v + 1) / 2 : -(int)(v / 2);
}

bool h264_sps_size(const uint8_t *au, int len, int *w, int *h)
{
    for (int i = 0; i + 4 < len; i++) {
        if (au[i] || au[i + 1] || au[i + 2] != 1 || (au[i + 3] & 0x1f) != 7) {
            continue;
        }
        // the SPS without its emulation-prevention bytes (00 00 03)
        uint8_t raw[64];
        int n = 0, zeros = 0;
        for (int j = i + 4; j < len && n < (int)sizeof(raw); j++) {
            if (zeros >= 2 && au[j] == 3) {
                zeros = 0;
                continue;
            }
            if (zeros >= 2 && au[j] <= 1) {
                break; // the next start code
            }
            zeros = au[j] ? 0 : zeros + 1;
            raw[n++] = au[j];
        }
        bits_t r = {raw, n, 0};
        int profile = get_bits(&r, 8);
        get_bits(&r, 16); // constraint flags, level
        get_ue(&r);       // sps id
        if (profile >= 100) {
            return false; // High profiles: the decoder can't do them anyway
        }
        get_ue(&r); // log2_max_frame_num - 4
        unsigned poc = get_ue(&r);
        if (poc == 0) {
            get_ue(&r);
        } else if (poc == 1) {
            get_bit(&r);
            get_se(&r);
            get_se(&r);
            for (unsigned k = get_ue(&r); k > 0 && k < 256; k--) {
                get_se(&r);
            }
        }
        get_ue(&r); // reference frames
        get_bit(&r);
        int mbw = get_ue(&r) + 1, mbh = get_ue(&r) + 1;
        int frame_mbs_only = get_bit(&r);
        if (!frame_mbs_only) {
            get_bit(&r);
        }
        get_bit(&r); // direct_8x8
        int cl = 0, cr = 0, ct = 0, cb = 0;
        if (get_bit(&r)) {
            cl = get_ue(&r);
            cr = get_ue(&r);
            ct = get_ue(&r);
            cb = get_ue(&r);
        }
        *w = mbw * 16 - 2 * (cl + cr);
        *h = mbh * 16 * (2 - frame_mbs_only) - 2 * (2 - frame_mbs_only) * (ct + cb);
        return *w > 0 && *h > 0;
    }
    return false;
}
