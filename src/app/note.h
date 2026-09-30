// One line of news for people: printed on serial and kept in a small ring
// that the web Status page shows (/api/log), so "what happened while nobody
// was watching" has an answer without a cable.
#pragma once

#include <string>
#include <vector>

namespace flapboard {

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
std::vector<std::string> recentNotes();

}  // namespace flapboard
