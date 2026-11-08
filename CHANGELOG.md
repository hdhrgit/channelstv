# Changelog

## Unreleased

- The centre 4:3 picture comes from the `esp32` profile with the bridge's overrides
  (`w=240&h=192&fit=crop43`); the bridge needs only the one client, and the `esp32_4x3` one can go.
- `/api/settings` takes `picture=16x9|4x3` instead of `profile=`; `/api/status` says
  `bridge.picture` and no longer `bridge.profiles`. The 4:3 choice starts over at 16:9.
- Needs: a bridge that takes the stream overrides, for the 4:3 picture.

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
