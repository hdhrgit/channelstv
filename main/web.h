// Web page and API on port 80:
//   GET  /                      status and settings
//   GET  /api/status            Wi-Fi, battery, memory, the bridge and playback
//   GET  /api/settings[?bridge=host[:port]][&profile=esp32|esp32_4x3][&volume=0..100]
//                               bridge= with nothing after it goes back to discovery
//   GET  /api/rescan            looks for the bridge again
//   GET  /api/browse?id=ID[&start=N]  a page of a ContentDirectory folder ("0" is the top)
//   GET  /api/play?url=U[&title=T][&live=1], /api/stop
//   GET  /api/seek[?by=±seconds]  where the recording that plays is; by= seeks it (needs a
//                               bridge that takes ?t=)
//   GET  /api/tv[?drop=1]       playback progress; drop=1 breaks the stream's connection (test)
//   GET  /api/touch[?gesture=tap|long|up|down|left|right&x=&y=]  acts as a finger would; the
//                               calibration and the last touch points, raw and mapped
//   GET  /api/screen            what the screen shows, as a 240x240 BMP (the way the board draws
//                               it, before the panel turns it)
//   GET  /api/rotation[?set=auto|0..3][&fake=x,y,z|off]  the screen's rotation (quarter turns
//                               clockwise);
//                               fake= stands in for the motion sensor (in g), for tests
//   POST /api/ota               raw app image: flash it to the spare slot and reboot
//   GET  /wifi                  the Wi-Fi setup page (also "/" while the setup hotspot is open)
//   GET  /api/wifi              the hotspot, the setup's progress, the network, the previous one
//   GET  /api/wifi/scan         the networks around
//   POST /api/wifi              form ssid=..&password=.. (or previous=1): join that network
//   POST /api/wifi/forget       forget the network and restart into the setup hotspot
#pragma once

void web_start(void);
