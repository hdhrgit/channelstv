# channelstv

Live TV and recordings from a [Channels DVR](https://getchannels.com/dvr-server/) on a small
ESP32-S3 touchscreen, through the [DLNA bridge for Channels DVR](https://github.com/hdhrgit/dlna-bridge).

The bridge converts each stream into something the board can decode (H.264 Constrained
Baseline, one slice per frame, up to 240x240, with mono AAC-LC audio). The board finds the
bridge on the network by itself, lists what the bridge serves, and plays it with sound.

## Board

[Waveshare ESP32-S3-Touch-LCD-1.54](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.54):
ESP32-S3 with 16 MB flash and 8 MB PSRAM, a 240x240 ST7789 touchscreen (CST816 touch), an ES8311
speaker codec, three keys and a Li-ion battery.

## Installing

Each [release](https://github.com/hdhrgit/channelstv/releases) has two images and their
checksums:

| File | What it is |
|------|------------|
| `channelstv-<version>-full.bin` | bootloader, partition table and app in one, to flash at `0x0` over USB |
| `channelstv-<version>-ota.bin` | the app alone, for updating a board that already runs channelstv |
| `SHA256SUMS` | `sha256sum -c SHA256SUMS` checks both |

**First install, over USB:** plug the board in (it shows up as `/dev/ttyACM0` on Linux,
`/dev/cu.usbmodem*` on macOS, a COM port on Windows) and flash the full image with
[esptool](https://docs.espressif.com/projects/esptool/) (`pip install esptool`):

```sh
esptool.py --chip esp32s3 -p /dev/ttyACM0 write_flash 0x0 channelstv-<version>-full.bin
```

If the port doesn't show up, hold BOOT while pressing RESET to put the board in download mode,
flash, then press RESET. The full image also covers the settings area, so the board starts
fresh: with no Wi-Fi network saved, it opens the setup hotspot (below). To keep the settings,
update over Wi-Fi instead.

**Updating over Wi-Fi:** send the OTA image to a board that already runs channelstv:

```sh
curl --data-binary @channelstv-<version>-ota.bin -H 'Content-Type: application/octet-stream' http://<board-ip>/api/ota
```

The board writes it to its spare app slot and restarts. If the new image can't get onto Wi-Fi
within 90 seconds, the board goes back to the previous one.

## Wi-Fi setup

With no network saved, the board opens its own hotspot, **`channelstv-setup`** (no password),
and shows a QR code for it:

1. Scan the QR code with a phone camera, or join `channelstv-setup` in the phone's Wi-Fi
   settings.
2. The setup page opens by itself. If it doesn't, open any `http://` address in the browser.
3. Pick the network, enter its password and tap **Join**. The page and the screen say how it's
   going ("Wrong password for ...?", "... not found").
4. Once the board is on the network, the page shows its address and the hotspot closes after
   30 seconds. Join your network again and open that address.

The network is saved. If it can't be reached two minutes after a start (a new router, a new
password), the hotspot opens again next to it. Meanwhile the board keeps trying the saved
network every 30 seconds.

To move the board to another network later, open `http://<board-ip>/wifi`. **Forget this
network** there (or `POST /api/wifi/forget`) clears the network and restarts into the hotspot.
The setup page then offers **Back to <network>** for the one just forgotten.

## Using it

- **Screen:** the bridge's folders (Live TV, Favorites, Recent recordings, TV Shows, Movies,
  Recording now, Channels by source) and Settings, four big rows a page; the page number is at
  the top right.
  - Swipe up for the next page, down for the previous one; swipe right (or tap the arrow at the
    top left) to go back.
  - Tap a folder to open it, a channel or a recording to play it. "Tuning... N s" covers the
    screen until the first picture (the DVR can take 15-30 s to start a channel; a tap there
    cancels).
  - In Live TV, a long press opens a number pad: type a channel number (`.` for subchannels) and
    tap ▶. Swipe left (or tap the heading) to delete a digit.
- **While playing:** tap the picture for the controls, a 3x3 grid that hides after 4 s: channel
  down / Info / channel up, volume down / Stop / volume up, Back / the title / Hide. Swipe up or
  down for the next or previous channel (the next or previous item of the folder), swipe right
  to stop.
- **Recordings:** with a bridge that can start a recording part-way (it lists `"seek"` in the
  `features` of its `/api/state`), the controls' volume keys become ⏮ 30 s and ⏭ 30 s; the
  volume stays on the VOL keys. Taps add up (three taps: 1:30) and the seek happens 0.8 s after
  the last one, showing where it lands ("+1:30", "→ 12:40 / 44:00"). Info shows how far the
  recording has got. A recording left part-way resumes there the next time it's played (the
  place is kept in NVS; within its first 30 s or last minute it starts from the top). Without
  that bridge feature, recordings play from the start to the end.
- **Settings:** Touch test (tap five dots; the board works out how the touch panel lines up with
  the screen and keeps it), the picture shape (16:9 or the centre 4:3), Find the bridge again,
  About.
- **Keys:** PWR stops playback, or goes back a folder. Hold PWR for 3 s to power off on battery.
  VOL+ and VOL- change the volume.
- **Web page:** `http://<board-ip>/` shows the status, the bridge it uses, a browser for the same
  folders and a picture of the screen. It also sets the bridge address, the picture shape and
  the volume. The API is listed in [`main/web.h`](main/web.h); `/api/touch` acts as a finger
  would (`?gesture=tap&x=..&y=..`, or `long`, `up`, `down`, `left`, `right`) and lists the last
  touch points, raw and mapped.

Live channels keep about 8 seconds in hand, so the gaps in the bridge's delivery don't stall the
picture. Anything beyond that is trimmed by playing 4 % faster (one audio frame in 25 left
out); if the board falls minutes behind, it skips ahead to what the bridge has just received.

## Finding the bridge

By default the board finds the bridge itself:

1. It sends an SSDP `M-SEARCH` for `urn:schemas-upnp-org:device:MediaServer:1` and keeps the
   answers whose `SERVER` header carries the bridge's `dlna/<version>` token.
2. It fetches each one's device description and accepts the first whose `manufacturerURL` is
   `https://github.com/hdhrgit/dlna-bridge`. Its address is taken from the description's URL.
3. It checks that the bridge's `/api/state` answers.

The description URL and the bridge's UDN are saved, so the next start tries there first. If
browsing or playing later fails to connect, the board searches again. If nothing answers, the
screen says so and the board searches again every 30 seconds.

**Setting the address by hand:** enter the bridge's address on the web page, for example
`192.0.2.10` or `my-dvr:8200` (the port is 8200 if left out). With an address set, the board
uses only that address and doesn't search. If that address doesn't answer, the screen says so.
Clear the field to go back to finding the bridge automatically.

## Bridge setup

The board asks for the client profile `esp32` (16:9, 240x144), or `esp32_4x3` (the centre 4:3
of the picture, 240x192) if that is chosen on the web page. It adds `?profile=<name>` to its
requests and sends the User-Agent `channelstv/<version>`. Without these clients the bridge sends
the DVR's original stream, which the board can't decode.

The board checks them whenever it finds the bridge: it reads the bridge's `/api/state` (it never
writes to the bridge) and wants each client to transcode to H.264 `baseline`, one slice a frame
(`single_slice`), at most 240 pixels wide. If the profile it asks for is missing or different,
it doesn't play. The screen says "Bridge needs the esp32 profile, see README" with the
reason, and so do the web page and `/api/status` (`bridge.profile_problem`, and
`bridge.profiles` for both). A stream that still won't decode ends with the same message.

Add the two clients on the bridge's settings page, or with `curl` and
[`jq`](https://jqlang.org/). `PUT /api/config` takes the whole configuration, so this reads the
current one from `/api/state`, puts the two clients in (replacing any with the same names) and
sends it back:

```sh
BRIDGE=http://<bridge-host>:8200
curl -s $BRIDGE/api/state | jq --argjson new '[
  {"name": "esp32", "user_agents": ["channelstv"],
   "profile": {"mode": "transcode", "ads": "cut",
     "transcode": {"max_width": 240, "max_height": 144, "fps": 24, "video_kbps": 250,
       "audio_kbps": 64, "audio_channels": 1, "h264_profile": "baseline",
       "pre_scale": true, "exact_size": true, "single_slice": true}}},
  {"name": "esp32_4x3",
   "profile": {"mode": "transcode", "ads": "cut",
     "transcode": {"max_width": 240, "max_height": 192, "fps": 24, "video_kbps": 250,
       "audio_kbps": 64, "audio_channels": 1, "h264_profile": "baseline",
       "pre_scale": true, "exact_size": true, "crop": "4:3", "single_slice": true}}}
]' '.config | .clients = ([.clients[]? | select(.name != "esp32" and .name != "esp32_4x3")] + $new)' |
curl -s -X PUT -H 'Content-Type: application/json' --data-binary @- $BRIDGE/api/config
```

Then tap **Find the bridge again** in Settings (or restart the board) so it checks again.
`"ads": "cut"` plays recordings without their ads; leave it out to keep them.

## Building

Releases are built by GitHub Actions ([`.github/workflows/build.yml`](.github/workflows/build.yml))
with ESP-IDF v5.4.2. Every push builds, and a `v*` tag publishes a release. The build is
reproducible (`CONFIG_APP_REPRODUCIBLE_BUILD`): the image carries no build paths or dates, and
[`tools/scan-artifacts.sh`](tools/scan-artifacts.sh) fails the build if an image contains
something that looks like an IP address, a LAN host name or an e-mail address.

To build it yourself, you need [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/v5.4.2/esp32s3/get-started/)
5.4. The other components (LVGL, the H.264 decoder and the audio codecs) come from the ESP
Component Registry on the first build.

```sh
. $IDF_PATH/export.sh
idf.py set-target esp32s3
idf.py menuconfig   # optional: channelstv -> Wi-Fi network and password
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

**Wi-Fi:** the board uses the network the Wi-Fi driver last saved. If none has been saved, it
uses the network set in menuconfig, and without one it opens the setup hotspot (see
[Wi-Fi setup](#wi-fi-setup)). You can also send `W<ssid>\t<password>\n` over the USB
console (for example `printf 'Wmy-network\tmy-password\n' > /dev/ttyACM0`). The board saves it
and connects.

**Updating over Wi-Fi:** `build/channelstv.bin` is the OTA image; send it as shown under
[Installing](#installing).

## Layout

| File | What it does |
|------|--------------|
| `main/main.c` | Start-up, Wi-Fi, keys, power management, rollback |
| `main/setup.c`, `main/setup.html` | The Wi-Fi setup hotspot, its DNS server and page |
| `main/board.c` | Board bring-up: display, touch, speaker codec, keys, battery |
| `main/bridge.c` | SSDP discovery, the bridge's description, ContentDirectory Browse (SOAP, DIDL-Lite) |
| `main/stream.c` | The HTTP stream: read-ahead ring, reconnects, catching up with the live edge |
| `main/ts.c` | MPEG-TS demuxer (PAT, PMT, PES) and the H.264 SPS size |
| `main/tv.c` | Playback: demuxer, decoder and draw tasks, A/V sync on the audio clock |
| `main/h264dec.c` | H.264 decoding (Espressif's `esp_h264`) and YUV to RGB565 conversion |
| `main/player.c` | AAC decoding, mono mix, resampling, the speaker |
| `main/ui.c` | LVGL screens |
| `main/web.c`, `main/index.html` | Web page and API |

The text font is DejaVu Sans (`main/DejaVuSans.ttf`), under its own free license.
