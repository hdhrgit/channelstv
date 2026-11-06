// Web page and API on port 80. Settings -> Web control (app.h): with read-only only GET / and
// GET /api/status (and POST /api/ota, under its own switch) answer, the rest 403; with off,
// nothing runs. While the setup hotspot is open its page and /api/wifi* always answer.
//   GET  /                      status and settings
//   GET  /api/status            Wi-Fi, battery, memory, the bridge and playback
//   GET  /api/settings[?bridge=host[:port]][&profile=esp32|esp32_4x3][&volume=0..100]
//                               bridge= with nothing after it goes back to discovery
//   GET  /api/rescan            looks for the bridge again
//   GET  /api/browse?id=ID[&start=N]  a page of a ContentDirectory folder ("0" is the top)
//   GET  /api/play?url=U[&title=T][&live=1][&id=ID&duration=S&seek=1], /api/stop  id, duration
//                               and seek (time_seek, from /api/browse) give a recording its
//                               seek keys, position and resume
//   GET  /api/seek[?by=±seconds]  where the recording that plays is; by= seeks it (a recording
//                               offered with DLNA time seek)
//   GET  /api/tv[?drop=1]       playback progress; drop=1 breaks the stream's connection (test)
//   GET  /api/touch[?gesture=tap|long|up|down|left|right&x=&y=]  acts as a finger would; the
//                               calibration and the last touch points, raw and mapped
//   GET  /api/screen            what the screen shows, as a 240x240 BMP (the way the board draws
//                               it, before the panel turns it)
//   GET  /api/rotation[?set=auto|0..3][&fake=x,y,z|off]  the screen's rotation (quarter turns
//                               clockwise);
//                               fake= stands in for the motion sensor (in g), for tests
//   POST /api/ota               raw app image: flash it to the spare slot and reboot (403 while
//                               Settings -> OTA updates is off; only a finger on the board turns
//                               it on, not /api/touch)
//   GET  /wifi                  the Wi-Fi setup page (also "/" while the setup hotspot is open)
//   GET  /api/wifi              the hotspot, the setup's progress, the network, the previous one
//   GET  /api/wifi/scan         the networks around
//   POST /api/wifi              form ssid=..&password=.. (or previous=1): join that network
//   POST /api/wifi/forget       forget the network and restart into the setup hotspot
#pragma once

// Starts or stops the web server as Web control says (app.h): running unless it's off, and
// always while the setup hotspot is open. Call again whenever either changes.
void web_apply(void);
