// One line of news for people: printed on serial and kept in a small ring
// that the web Status page shows (/api/log), so "what happened while nobody
// was watching" has an answer without a cable.
#pragma once

#include <string>
#include <vector>

namespace flapboard {

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
std::vector<std::string> recentNotes();
// While a binary transfer (screenshot, file get/put) owns the serial port,
// notes still go to the ring but not to serial: a line printed from another
// task in the middle of a screenshot shifted the rest of the picture.
void setSerialQuiet(bool quiet);

}  // namespace flapboard
