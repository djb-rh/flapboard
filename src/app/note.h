// One line of news for people: printed on serial and kept in a small ring
// that the web Status page shows (/api/log), so "what happened while nobody
// was watching" has an answer without a cable.
#pragma once

#include <string>
#include <vector>

namespace flapboard {

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// Serial only, kept out of the web page's activity log (frequent diagnostics).
void trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
std::vector<std::string> recentNotes();
// The last ~4 KB of notes from before this start (software restart, watchdog
// or crash; not a power cut), kept in RAM that a reset doesn't clear.
std::vector<std::string> previousNotes();
// While a binary transfer (screenshot, file get/put) owns the serial port,
// notes still go to the ring but not to serial: a line printed from another
// task in the middle of a screenshot shifted the rest of the picture.
void setSerialQuiet(bool quiet);

}  // namespace flapboard
