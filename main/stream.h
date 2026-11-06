// A TV stream from the bridge: a plain GET (no Range, it can't seek) read ahead into a PSRAM
// ring by a fetcher task. A live channel is endless and the fetcher reconnects when it stalls
// or breaks; a recording ends, and a broken one fails (starting again would go back to its
// beginning). The bridge runs only so many conversions at once and answers one more with
// HTTP 503 ("busy").
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct stream stream_t;
enum {
    STREAM_PACKET = 1,    // a 188-byte TS packet came out
    STREAM_TIMEOUT = 0,   // nothing yet
    STREAM_RESTART = 2,   // a new connection: the stream starts over (new timestamps, from an IDR)
    STREAM_FAILED = -1,   // gave up; stream_error() says why
    STREAM_CAUGHT_UP = 3, // after stream_catch_up(): what was buffered is gone, the stream goes on
                          // from the bridge's live edge (mid-packet: a fresh start, from an IDR)
    STREAM_END = 4,       // a recording reached its end
};
stream_t *stream_open(const char *url, bool live);
int stream_read(stream_t *s, uint8_t pkt[188], int timeout_ms);
const char *stream_error(stream_t *s);
bool stream_busy(stream_t *s); // failed because the bridge already runs all the conversions it allows
// Catching up with the live edge: the fetcher reads and throws away what the bridge has
// waiting, as fast as Wi-Fi goes, until a read has to wait; the ring's unread bytes go too.
// The reader then gets STREAM_CAUGHT_UP. Returns at once.
void stream_catch_up(stream_t *s);
bool stream_catching_up(stream_t *s);   // asked for and not done yet
uint32_t stream_fill(stream_t *s);      // bytes in the ring, unread
int stream_full_ms(stream_t *s);        // how long the ring has stayed at least 3/4 full (0 if not)
uint32_t stream_caught_up_bytes(void);  // what the latest catch-up threw away
// Test hook (/api/tv?drop=1): the playing stream's connection is dropped as if it broke.
void stream_test_drop(void);
// Returns at once; the fetcher task closes the connection and frees everything once its
// current wait returns (<= 1 s). The reader calls it, once.
void stream_close(stream_t *s);
// {"connects":..,"reconnects":..,"busy":..,"bytes":..,"first_byte_ms":..,"state":..,"last_error":..}
void stream_status_json(char *out, size_t len);
