# Changelog

## Unreleased

- README: the AMOLED-1.8 V1 (SH8601 and FT3168) is untested; it has its own code path but
  hasn't been tried on that board. The README also describes v0.5.0's picture options, frame
  rate and upscaling (AMOLED-1.8 only), the `/api/settings` parameters and the Wi-Fi setup
  hotspot as they are now.

## v0.5.0 (2026-11-12)

- The Waveshare ESP32-S3-Touch-AMOLED-1.8 (V1: SH8601 and FT3168; V2: CO5300 and CST816), with
  the same image as the 1.54: the board is told apart at start-up. Its panel, touch, AXP2101
  power chip (battery, charging, power-off), IO expander and keys are set up for it.
- On the AMOLED-1.8 the menus are scaled to its screen, six rows a page under a heading as tall as
  a row. The board asks the bridge for its screen's size and lets the bridge fit the picture
  (`fit=width`, or `fit=height` for a picture cropped to the screen's shape), so the picture is
  drawn one pixel per pixel.
- On the AMOLED-1.8 video turns sideways with the board, across the panel's whole height; the
  menus stay upright. Video frames are written behind the panel's refresh, timed by its
  tearing-effect line; `/api/settings?tesync=0|1` switches that until the next start.
- AMOLED-1.8 Picture setting: 16:9, 4:3 centre, or Fill screen (the middle of the picture,
  cropped to the screen's shape: the whole screen sideways). Also on the web page and in
  `/api/settings` (`picture=16x9|4x3|full`).
- AMOLED-1.8 Upscaling setting: off, 1.5x or 2x. The board asks the bridge for a smaller picture
  and scales it up, for a smoother frame rate on a busy channel. Also on the web page and in
  `/api/settings` (`scale=1|1.5|2`).
- AMOLED-1.8 keys: PWR stops playback or goes back, held 3 s powers off, and switches the board on;
  BOOT also goes back, and held steps the volume down and round, stopping at 50 % the second time.
- H.264 decoding with [VideoJet](https://github.com/hdhrgit/VideoJet) (Apache-2.0), fetched by
  the component manager from git, pinned to a commit; `esp_h264` stays selectable in
  menuconfig. `/api/perf` shows the decoder's timings.
- Settings → Version & board: the board, its display and each chip with its ID, the firmware build,
  the Wi-Fi signal and channel, the battery, free memory, uptime and why it last started. The web
  page links a Board and chips page (`/chips`, `/api/chips`); `/api/status` names the board.
- A quick flick slides through many pages, with the page number large, and fetches only the page
  it stops on; a tap stops it.
- The last page of a folder shows its last full page of rows, and coming back up from a folder
  keeps each folder's own count.
- A page turn keeps the rows on the screen until the next page comes, instead of blanking them.
- The heading's page number, battery and Wi-Fi take only the room their text needs.
- Wi-Fi joins the strongest access point of its network, moves to a much stronger one when weak
  and idle, and the signal shown is smoothed.
- A recording whose stream ends before its last minute keeps its place to resume from.
- AMOLED-1.8 Frame rate setting: 24, 20 or 15 fps (default 15), also on the web page and in
  `/api/settings` (`fps=`). The board asks the bridge for that rate (`fps=` on every stream
  request). The 1.54 always asks for 24 fps and has no Frame rate or Upscaling setting.
- Faster video on both boards: the decoder hands its frame over while it decodes the next, the
  colour conversion and the sideways turn use the chip's vector instructions, and the drawing
  runs from internal memory.
- A board that can't keep up with a stream slows its picture down instead of dropping it: a late
  frame still shows when no newer one is waiting, and half a second behind it skips to the next
  keyframe. The web page keeps answering while it does.
- A slow stream no longer freezes the picture at its first frame.
- The AMOLED-1.8 checks that the bridge can fit its picture (the fits in the bridge's
  `/api/caps`, or its version where it has none). With an older bridge it plays nothing and
  says "Update dlna-bridge to 0.99.12 or later", on the screen, on the web page and in
  `/api/status` (`bridge.update`).
- The Wi-Fi setup hotspot (with its QR code) opens two minutes after start only if the board
  hasn't been on its network at all since. The board keeps its saved network and keeps trying
  it, and leaves the setup by itself when it joins. A network lost later is only rejoined.
- `/api/perf` also reports the largest free block of PSRAM (`psram_largest`).
- Builds without warnings.
- Needs: on the AMOLED-1.8, dlna-bridge 0.99.12 or later (for `fit=width` and `fit=height`), with
  0.99.13 or later recommended when several players watch the same channel. The 1.54 asks the
  bridge as before and works with older bridges (v0.99.10 or later).

## v0.4.1 (2026-11-12)

- With no bridge found (or not answering), the top screen still has a Settings row under the
  note, so Web control can be set to full and the bridge's address entered on the web page.
- With no bridge found, the note says to install dlna-bridge.

## v0.4.0 (2026-11-08)

- The centre 4:3 picture comes from the `esp32` profile with the bridge's stream overrides
  (`w=240&h=192&fit=crop43`); the bridge needs only the `esp32` client, and the `esp32_4x3` one
  can go.
- `/api/settings` takes `picture=16x9|4x3` instead of `profile=`; `/api/status` says
  `bridge.picture` and no longer `bridge.profiles`. The 4:3 choice starts over at 16:9.
- The screen refreshes at 48 Hz, twice the video's 24 fps. The picture is turned in software,
  so the panel is always written along its own scan, whichever way the board is held.
- While charging, the heading shows the battery level next to the charge symbol. On the charger
  the voltage reads high, so it maps through a charging curve: the level only rises and stays
  at 99 % until charging ends.
- Settings: Backlight (20 to 100 %, default 80), and on battery Dim (30 s, 1, 5 or 10 min, or
  never; default 1 min) and Off (1, 5, 10 or 30 min, or never; default 5 min), also on the web
  page and in `/api/settings` (`brightness=`, `dim=`, `off=`).
- The dim timer starts when USB power goes, so unplugging no longer turns the screen off at
  once; plugging in doesn't wake a dimmed or dark screen.
- Settings: About is now Version, with the version on the row; tapping it opens a page headed
  with the version, showing the board's address and the bridge's address, version, how it was
  found and its whole name (a long name scrolls once). The web page shows both versions, and
  `/api/status` the bridge's.
- The heading's title no longer runs into the page number, and the bridge's name scrolls once,
  slowly, when the top menu appears.
- The board accepts the `esp32` profile built into newer bridges when the bridge's settings have
  no `esp32` client of their own.
- Needs: dlna-bridge v0.99.10 or later, for the 4:3 picture.

## v0.3.0 (2026-11-07)

- Web page: search the titles of recordings, movies and shows, with the bridge's UPnP Search
  where it has one (every episode too), else the page's own; a result plays on the board (a
  recording with its seek keys and position) or opens the show's episodes.
- Resume shared with the DVR's other players: a recording starts where the bridge says it was
  left (`upnp:lastPlaybackPosition`, H:MM:SS(.mmm) or seconds), else where the board left it.
- Recordings not watched yet (`upnp:playbackCount` 0) carry a dot, on the screen and the page.
- README: the web page and the API, with what answers while Web control is read-only.
- Builds leave out LVGL's bundled examples and demos.
- Needs: search, the shared resume and the dots need a bridge that offers them; older bridges
  still work (the page then searches Movies, Recent recordings and TV Shows itself).

## v0.2.1 (2026-11-06)

- Settings → Web control: full, read-only (the default, also after updating) or off. Read-only
  serves the status page and `/api/status` only; off runs no web server. Only a finger on the
  board loosens it. OTA updates keep their own switch; the setup hotspot's page always works.
- The battery in the screen's heading: percentage and symbol (the charge symbol while charging),
  red at 15 % and under; `/api/status` says whether a battery reads.
- The heading's title, page number and battery no longer cut or wrap each other (the number
  pad's typed channel shows again).
- README: a demo GIF of the board.

## v0.2.0 (2026-11-06)

- Release images from GitHub Actions: `full.bin` (first install at `0x0`, USB or the browser),
  `ota.bin` (updates over Wi-Fi) and `SHA256SUMS`; reproducible builds.
- First boot: the board opens the hotspot `channelstv-setup` with a QR code to join it; its page
  picks the Wi-Fi network. The hotspot also opens if the saved network is gone for two minutes.
- The board checks the bridge's `esp32` and `esp32_4x3` client profiles and says what's wrong
  instead of a black screen.
- Recordings: ⏮ 30 s / ⏭ 30 s in the playback controls (DLNA time seek), a position bar in
  Info, and resume where a recording was left.
- The screen turns with the board (auto, or a fixed rotation in Settings).
- OTA updates are off until turned on in Settings on the board.

Needs: the DLNA bridge for Channels DVR with the `esp32` client profiles (README, Bridge setup);
seeking needs the bridge's client set to seek by time.

Limits: the Waveshare ESP32-S3-Touch-LCD-1.54 only; the setup hotspot is open (no password)
while it runs; the web page and API have no login.

## v0.1.0

- Finds the bridge, browses its folders and plays live TV and recordings with sound.
