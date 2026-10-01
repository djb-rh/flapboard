// Wall-clock time: the RX8130 RTC carries it across power-offs, NTP keeps it
// right, and the configured IANA time zone (mapped to a POSIX rule from the
// built-in table) turns UTC into local time, daylight saving included.
#pragma once

#include <ctime>
#include <string>

namespace flapboard {
namespace clock {

void begin();   // main loop, after config::begin(): system time from the RTC if it was ever set
void loop();    // main loop: starts NTP once Wi-Fi is up, writes the RTC after a sync, follows TZ changes

// True once the time is known good: NTP synced this boot, or the RTC was set
// by NTP at some point and still holds a sane year.
bool trusted();
bool localNow(struct tm *out);   // false (and *out zeroed) when not trusted
std::string zone();              // the IANA name in use ("UTC" if unknown)
std::string posixFor(const std::string &iana);   // "" if not in the table
std::string statusText();        // "synced 2 min ago", "from RTC", "not set"

}  // namespace clock
}  // namespace flapboard
