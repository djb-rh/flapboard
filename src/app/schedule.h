// What to show and whether to be on, decided from the settings and the
// time. Ported from the Pi frame's scheduler.py, keeping its rules:
//
//   one-off date overrides  >  weekly rules  >  the default program
//
// first match wins within each layer; windows may cross midnight; a rule
// with a broken time matches nothing (not "all day"); a rule with no
// program is skipped; nothing runs until the clock is known good.
//
// One deliberate difference: the after-midnight part of a window that
// crosses midnight belongs to the day it started. "Fridays 22:00-06:00"
// still matches at 02:00 on Saturday (the Pi checked Saturday and missed it).
//
// Plain C++ (tested on the Mac); the app turns its JSON settings into these.
#pragma once

#include <string>
#include <vector>

namespace flapboard {
namespace schedule {

struct Program {
  std::string source;               // messages | clock | weather | text (| photos later); "" = none
  std::vector<std::string> files;   // messages: which files (empty = all)
  std::string order = "random";
  std::string text;                 // text: the message
  int dwell = 20;                   // seconds per message
  bool empty() const { return source.empty(); }
};

struct Rule {
  std::string name;
  std::vector<int> days;   // weekly rules: 0 = Monday ... 6 = Sunday; empty = every day
  std::string date;        // overrides: "YYYY-MM-DD"
  std::string start, end;  // "HH:MM"; both empty = all day
  Program program;         // unused for sleep rules
};

struct Now {
  int year = 0, month = 0, day = 0;   // local date
  int weekday = 0;                    // 0 = Monday
  int minutes = 0;                    // since local midnight
};

struct Choice {
  Program program;
  std::string reason;   // "default", "rule: Breakfast", "override: Christmas", "waiting for the clock"
  int kind = 0;         // 0 default, 1 weekly rule, 2 override
  int index = -1;
};

int parseHHMM(const std::string &s);   // minutes, or -1
bool inWindow(int now, int start, int end);
// Does the rule's day and time window cover now? (Overrides compare dates,
// weekly rules weekdays; the after-midnight part of a window counts as the
// previous day's.)
bool matches(const Rule &r, const Now &now, bool override_rule);

Choice chooseProgram(const Program &fallback, bool enabled, const std::vector<Rule> &rules,
                     const std::vector<Rule> &overrides, bool clock_ok, const Now &now);

// Whether the display (and the car's lights) should be on, and why.
struct PowerInput {
  bool latch_off = false;      // Home Assistant / the web page said off (outranks everything)
  bool sleep_enabled = false;
  std::vector<Rule> sleep;     // hours the sign is OFF
  bool clock_ok = false;
  Now now;
  bool woken = false;          // a tap (or, later, motion) asked for light just now
  int motion = -1;             // -1 not in use, 0 no motion for the timeout, 1 recent motion
};
struct Power {
  bool on = true;
  std::string reason;
};
Power choosePower(const PowerInput &in);

std::string describe(const Rule &r);   // "Mon,Tue 07:00-10:30" / "every day all day"

}  // namespace schedule
}  // namespace flapboard
