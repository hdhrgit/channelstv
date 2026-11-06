// Wi-Fi setup. With no network saved (or none reachable two minutes after start), the board opens
// the hotspot SETUP_SSID (no password) next to its station. A DNS server on the hotspot answers
// every name with the board's address, so a phone that joins lands on the setup page (web.c),
// which lists the networks around and joins the one picked. Once the board is on it, the hotspot
// closes; if it doesn't take, main.c puts back the network saved before.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define SETUP_SSID "channelstv-setup"
// what the screen's QR code holds: a phone camera offers to join the hotspot
#define SETUP_QR   "WIFI:T:nopass;S:" SETUP_SSID ";;"

// The hotspot and its DNS server. Wi-Fi must be started. saved: the network saved but not
// reachable, which the board keeps trying every 30 s; NULL with none.
void setup_start(const char *saved);
bool setup_active(void);
// The hotspot's address, empty while it is off.
void setup_ap_ip(char *out, size_t len);
// What the setup is doing, for the screen and the page: "" while it waits for a network to be
// picked, then "Connecting to X...", "Wrong password for X?", "X not found", "Connected to X".
void setup_status(char *out, size_t len);
bool setup_failed(void);
// Changes whenever the hotspot opens or closes, or the status changes.
unsigned setup_gen(void);

// From main.c's Wi-Fi events.
void setup_on_connecting(const char *ssid);
// When to try again, in ms; -1: not until another network is picked (the status says why).
int setup_on_disconnected(int reason);
void setup_on_got_ip(const char *ip);

// The networks around as JSON, [{"ssid":"..","rssi":-50,"secure":true}, ...], strongest first,
// each name once; NULL if the radio is busy (connecting). free() it.
char *setup_scan_json(void);
