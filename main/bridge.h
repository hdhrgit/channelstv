// The DLNA bridge for Channels DVR (github.com/hdhrgit/dlna-bridge): finding it on the network,
// and browsing what it serves through its UPnP ContentDirectory.
//
// Finding it: unless an address is set by hand, an SSDP M-SEARCH for MediaServer:1 devices,
// and the first whose description names the bridge's manufacturerURL (and whose /api/state
// answers) is the one. Its description URL and UDN are kept in NVS so the next boot tries
// there first; a failure later on (a browse or a stream that can't connect) searches again.
// The board talks to one bridge only.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define BRIDGE_MANUFACTURER_URL "https://github.com/hdhrgit/dlna-bridge"

typedef enum {
    BRIDGE_WAITING,   // for Wi-Fi
    BRIDGE_SEARCHING,
    BRIDGE_FOUND,
    BRIDGE_NOT_FOUND, // nothing answered; searched again every 30 s
    BRIDGE_UNREACHABLE, // the address set by hand doesn't answer
} bridge_state_t;

// Reads the settings from NVS and starts the background task that finds the bridge.
// online() says whether Wi-Fi is up.
void bridge_start(bool (*online)(void));

bridge_state_t bridge_state(void);
const char *bridge_state_text(void);
// "http://192.0.2.10:8200", empty until found
void bridge_base(char *out, size_t len);
const char *bridge_name(void);    // its friendlyName
const char *bridge_found_by(void); // "discovery", "saved", "manual" or ""
// Changes whenever the bridge is found again or lost.
unsigned bridge_gen(void);
// Searches again (or tries the manual address again), in the background.
void bridge_rescan(void);

// The address set by hand ("192.0.2.10" or "my-dvr:8200"), empty for discovery. Saved in NVS.
const char *bridge_override(void);
void bridge_set_override(const char *host_port);
// The picture the board asks the bridge for: "16x9" (the esp32 profile's 240x144) or "4x3" (the
// centre 4:3 of the picture, 240x192, with the bridge's w, h and fit overrides). Saved in NVS.
const char *bridge_picture(void);
void bridge_set_picture(const char *shape);
// The picture size the board asks the bridge for (0 x 0: the profile's), and a different one
// to try until the next restart.
void bridge_size(int *w, int *h);
void bridge_set_size(int w, int h);
// The frame rate the board asks the bridge for on every stream: 15, 20 or 24. Saved in NVS; until
// set, 24 on a 240x240 panel and 15 on a larger one, where decoding has more to do.
int bridge_fps(void);
void bridge_set_fps(int fps);
// Upscaling on the larger panel, in tenths: 10 (none), 15 or 20. The board asks the bridge for
// its picture size divided by it (the width a multiple of 8) and scales the picture back up.
// Saved in NVS; always 10 on the 240x240 panel.
int bridge_scale(void);
void bridge_set_scale(int tenths);

typedef struct {
    char id[80];
    char title[96];
    bool container;
    int children;     // a container's childCount, -1 if it didn't say
    bool live;        // a channel (upnp:class ...videoBroadcast): endless
    char url[256];    // an item's stream, with the profile and picture asked for
    int duration_s;   // a recording's length (without its ads if they are cut), 0 if unknown
    bool time_seek;   // offered with DLNA time seek (DLNA.ORG_OP=1x): it starts where a
                      // TimeSeekRange.dlna.org request header says
    int last_pos_s;   // upnp:lastPlaybackPosition: where it was left, shared with the DVR's other
                      // players; -1 if not given
    int play_count;   // upnp:playbackCount: 0 unwatched; -1 if not given
} cd_entry_t;

// Browses the children of container `id` ("0" is the top), from `start`, at most `max`.
// Blocks for up to a few seconds (call from a background task). Returns how many came back
// and sets *total, or -1 if the bridge couldn't be reached (and searches for it again).
int bridge_browse(const char *id, int start, int max, cd_entry_t *out, int *total);

// Searches the bridge's recordings, movies and shows (UPnP Search; not live channels) for titles
// holding every word of words, ignoring case. Same answer as bridge_browse(); -2 if the bridge has no Search.
int bridge_search(const char *words, int start, int max, cd_entry_t *out, int *total);

// What is wrong with the bridge's esp32 client profile: no such
// client, not transcoding, not H.264 baseline, not one slice a frame, wider than 240. False if
// it's fine, or the bridge's settings couldn't be read. Checked whenever the bridge is found.
bool bridge_profile_problem(char *out, size_t len);
// Reads the bridge's client profile again, now (blocks for up to 4 s); true if it is wrong.
bool bridge_recheck_profiles(void);
// The message the screen and the web page show for a wrong profile
#define BRIDGE_PROFILE_HELP "Bridge needs the esp32 profile, see README"
// The oldest bridge that fits the picture as the board asks (fit=width / fit=height): an older one
// counts as a profile problem, with this message instead
#define BRIDGE_MIN_VERSION "0.99.12"
#define BRIDGE_UPDATE_HELP "Update dlna-bridge to " BRIDGE_MIN_VERSION " or later"
// BRIDGE_UPDATE_HELP when the profile problem is the bridge's version, else BRIDGE_PROFILE_HELP
const char *bridge_problem_help(void);

// The bridge's own version ("" until it's known), read with its client profiles.
void bridge_version(char *out, size_t len);

// {"state":..,"base":..,"name":..,"found_by":..,"override":..,"picture":..,"udn":..,"version":..,
//  "profile_problem":"" or why, "update":null or the bridge version needed}
void bridge_status_json(char *out, size_t len);
