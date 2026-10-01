// What the sign shows: the message library on the card, the clock, the
// weather or a fixed text, with fill-in fields expanded every second (so a
// clock only flips the digits that changed). Phase 6's scheduler chooses
// among programs; for now the default program comes from the settings.
//
// Message files (/flapboard/messages/*.txt): messages are separated by blank
// lines; lines starting with # are comments; a message may start with an
// options line: "@left", "@right", "@center", "@top", "@hold 45" (seconds).
#pragma once

#include <string>
#include <vector>

#include "content_parse.h"

namespace flapboard {
namespace content {

void begin();   // main loop, after the SD card and config
void loop();    // main loop: about once a second

// A message from outside (the web page's "show now", MQTT later): shown for
// `seconds` (0 = until another one or clearOverride()), then the program resumes.
void showOverride(const std::string &text, int seconds);
void clearOverride();
void next();                  // move on to the next message now (Home Assistant's button)
std::string overrideText();   // the message from outside on top right now, or ""
void libraryChanged();   // a message file was edited or uploaded

std::string statusJson();
// Expands {fields} as the sign would right now (for the web preview).
std::string expand(const std::string &tpl);

}  // namespace content
}  // namespace flapboard
