#include "schedule.h"

#include <cstdio>

namespace flapboard {
namespace schedule {
namespace {

std::string trim(const std::string &s) {
  size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

bool hasDay(const std::vector<int> &days, int d) {
  if (days.empty()) return true;
  for (int x : days)
    if (x == d) return true;
  return false;
}

// The calendar day before now (for the after-midnight part of a window).
Now previousDay(const Now &n) {
  static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  Now p = n;
  p.weekday = (n.weekday + 6) % 7;
  if (--p.day < 1) {
    if (--p.month < 1) {
      p.month = 12;
      p.year--;
    }
    const bool leap = (p.year % 4 == 0 && p.year % 100 != 0) || p.year % 400 == 0;
    p.day = kDays[p.month - 1] + (p.month == 2 && leap ? 1 : 0);
  }
  return p;
}

std::string dateOf(const Now &n) {
  char b[16];
  snprintf(b, sizeof(b), "%04d-%02d-%02d", n.year, n.month, n.day);
  return b;
}

bool dayMatches(const Rule &r, const Now &n, bool override_rule) {
  return override_rule ? trim(r.date) == dateOf(n) : hasDay(r.days, n.weekday);
}

}  // namespace

int parseHHMM(const std::string &s_in) {
  const std::string s = trim(s_in);
  int h, m;
  char extra;
  if (sscanf(s.c_str(), "%d:%d%c", &h, &m, &extra) != 2) return -1;
  if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
  return h * 60 + m;
}

bool inWindow(int now, int start, int end) {
  if (start == end) return false;   // zero length: a mistake, so it matches nothing (visibly)
  if (start < end) return now >= start && now < end;
  return now >= start || now < end;   // crosses midnight
}

bool matches(const Rule &r, const Now &n, bool override_rule) {
  const bool has_start = !trim(r.start).empty(), has_end = !trim(r.end).empty();
  if (!has_start && !has_end) return dayMatches(r, n, override_rule);   // all day
  const int s = parseHHMM(r.start), e = parseHHMM(r.end);
  if (s < 0 || e < 0) return false;   // present but broken: matches nothing, never "all day"
  if (!inWindow(n.minutes, s, e)) return false;
  // Crossing midnight and now in the morning part: that is the previous
  // day's window.
  if (s > e && n.minutes < e) return dayMatches(r, previousDay(n), override_rule);
  return dayMatches(r, n, override_rule);
}

Choice chooseProgram(const Program &fallback, bool enabled, const std::vector<Rule> &rules,
                     const std::vector<Rule> &overrides, bool clock_ok, const Now &now) {
  Choice c;
  c.program = fallback;
  if (!enabled) {
    c.reason = "schedule off";
    return c;
  }
  if (!clock_ok) {   // normal for a minute after a first boot; never act on a wrong clock
    c.reason = "waiting for the clock";
    return c;
  }
  for (size_t i = 0; i < overrides.size(); i++) {
    const Rule &r = overrides[i];
    if (r.program.empty() || !matches(r, now, true)) continue;
    c.program = r.program;
    c.reason = "override: " + (r.name.empty() ? trim(r.date) : r.name);
    c.kind = 2;
    c.index = (int)i;
    return c;
  }
  for (size_t i = 0; i < rules.size(); i++) {
    const Rule &r = rules[i];
    if (r.program.empty() || !matches(r, now, false)) continue;
    c.program = r.program;
    c.reason = "rule: " + (r.name.empty() ? describe(r) : r.name);
    c.kind = 1;
    c.index = (int)i;
    return c;
  }
  c.reason = "default";
  return c;
}

Power choosePower(const PowerInput &in) {
  Power p;
  if (in.latch_off) {
    p.on = false;
    p.reason = "turned off remotely";
    return p;
  }
  if (in.woken) {
    p.reason = "woken by a tap";
    return p;
  }
  if (in.sleep_enabled && !in.sleep.empty()) {
    if (!in.clock_ok) {
      // Fail towards light: a sign that boots inside its own sleep hours,
      // or whose clock never syncs, must not sit dark looking broken.
      p.reason = "on (sleep hours wait for the clock)";
      if (in.motion != 0) return p;
    } else {
      for (const Rule &r : in.sleep) {
        // A sleep rule must have both times: an all-day one would mean
        // "never on", which nobody sets on purpose.
        if (parseHHMM(r.start) < 0 || parseHHMM(r.end) < 0) continue;
        if (!matches(r, in.now, false)) continue;
        p.on = false;
        p.reason = "asleep: " + (r.name.empty() ? describe(r) : r.name);
        return p;
      }
    }
  }
  if (in.motion == 0) {
    p.on = false;
    p.reason = "no motion lately";
    return p;
  }
  p.reason = in.motion == 1 ? "motion seen" : "on";
  return p;
}

std::string describe(const Rule &r) {
  static const char *const kNames[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
  std::string days;
  if (!r.date.empty()) days = trim(r.date);
  else if (r.days.empty() || r.days.size() >= 7) days = "every day";
  else
    for (int d = 0; d < 7; d++)
      if (hasDay(r.days, d)) days += (days.empty() ? "" : ",") + std::string(kNames[d]);
  const bool timed = !trim(r.start).empty() && !trim(r.end).empty();
  return days + (timed ? " " + trim(r.start) + "-" + trim(r.end) : " all day");
}

}  // namespace schedule
}  // namespace flapboard
