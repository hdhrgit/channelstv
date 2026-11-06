# Changelog

## v0.2.0

- Release images from GitHub Actions: `full.bin` (first install at `0x0`, USB or the browser),
  `ota.bin` (updates over Wi-Fi) and `SHA256SUMS`; reproducible builds.
- First boot: the board opens the hotspot `channelstv-setup` with a QR code to join it; its page
  picks the Wi-Fi network. The hotspot also opens if the saved network is gone for two minutes.
- The board checks the bridge's `esp32` and `esp32_4x3` client profiles and says what's wrong
  instead of a black screen.
- Recordings: a position bar in Info.
- The screen turns with the board (auto, or a fixed rotation in Settings).
- OTA updates are off until turned on in Settings on the board.
- Ready for seeking (30 s keys, resume where left) once the bridge has seek support.

Needs: the DLNA bridge for Channels DVR with the `esp32` client profiles (README, Bridge setup);
seeking needs a bridge with seek support (coming).

Limits: the Waveshare ESP32-S3-Touch-LCD-1.54 only; the setup hotspot is open (no password)
while it runs; the web page and API have no login.

## v0.1.0

- Finds the bridge, browses its folders and plays live TV and recordings with sound.
