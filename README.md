# channelstv

Live TV and recordings from a [Channels DVR](https://getchannels.com/dvr-server/) on a small
ESP32-S3 touchscreen, through the [DLNA bridge for Channels DVR](https://github.com/hdhrgit/dlna-bridge).

The bridge converts each stream into something the board can decode (H.264 Constrained
Baseline, one slice per frame, up to 240x240, with mono AAC-LC audio). The board finds the
bridge on the network by itself, lists what the bridge serves, and plays it with sound.

## Board

[Waveshare ESP32-S3-Touch-LCD-1.54](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.54):
ESP32-S3 with 16 MB flash and 8 MB PSRAM, a 240x240 ST7789 touchscreen (CST816 touch), an ES8311
speaker codec, three keys and a Li-ion battery.

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
Recordings play from the start to the end, without seeking.

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

## Bridge profiles

The board asks for the client profile `esp32` (16:9, 240x144), or `esp32_4x3` (the centre 4:3
of the picture, 240x192) if that is chosen on the web page. It adds `?profile=<name>` to its
requests and sends the User-Agent `channelstv/<version>`. Set up these clients on the bridge's
settings page:

| Client      | Matches                                   | Settings |
|-------------|-------------------------------------------|----------|
| `esp32`     | `?profile=esp32`, User-Agent `channelstv` | transcode, 240x144 exact size, 24 fps, H.264 baseline, single slice, 250 kbps video, 64 kbps mono audio, pre-scale |
| `esp32_4x3` | `?profile=esp32_4x3`                      | the same, with a 4:3 crop at 240x192 |

Without them the bridge sends the DVR's original stream, which the board can't decode.

## Building

You need [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/v5.4.2/esp32s3/get-started/)
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
uses the network set in menuconfig. You can also send `W<ssid>\t<password>\n` over the USB
console (for example `printf 'Wmy-network\tmy-password\n' > /dev/ttyACM0`). The board saves it
and connects.

**Updating over Wi-Fi:** once channelstv is running, a new build can go over the network:

```sh
curl --data-binary @build/channelstv.bin -H 'Content-Type: application/octet-stream' http://<board-ip>/api/ota
```

The board writes it to its spare app slot and restarts. If the new image can't get onto Wi-Fi
within 90 seconds, the board goes back to the previous one.

## Layout

| File | What it does |
|------|--------------|
| `main/main.c` | Start-up, Wi-Fi, keys, power management, rollback |
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
