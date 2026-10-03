// The one on/off decision for the sign and the train car's lights. Every
// quarter second it asks schedule::choosePower and applies the answer to the
// backlight, the drawing, the sound and the relay pin together, so the
// display and the lights can never disagree.
//
//   "off" from Home Assistant / the web  (held, survives a reboot)
//   > a tap on the dark screen (lights it for tap_wake_minutes)
//   > sleep hours
//   > motion (Phase 7)
//   > on
#pragma once

#include <string>

namespace flapboard {
namespace power {

void beginEarly();   // first thing in setup(), after config::begin(): drive the relay OFF
void begin();        // after M5.begin()
void loop();         // main loop (backlight and NVS are main-loop work)

bool isOn();
// The screen is dark because a firmware update is arriving (web.cpp waits for it).
bool blankedForUpdate();
void wake();                    // a tap while dark
void requestLatch(bool off);    // any task: true = "off" (held), false = release ("on")
void testRelay(int flips);   // flip the relay the other way and back, a second each, this many times
std::string statusJson();

}  // namespace power
}  // namespace flapboard
