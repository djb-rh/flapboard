#include <unity.h>

#include "../../src/app/schedule.h"

using namespace flapboard::schedule;

static Now at(int y, int mo, int d, int wd, int h, int mi) {
  Now n;
  n.year = y; n.month = mo; n.day = d; n.weekday = wd; n.minutes = h * 60 + mi;
  return n;
}
static Program prog(const char *src) {
  Program p;
  p.source = src;
  return p;
}
static Rule rule(const char *name, std::vector<int> days, const char *s, const char *e, const char *src) {
  Rule r;
  r.name = name; r.days = days; r.start = s; r.end = e; r.program = prog(src);
  return r;
}

void test_times_and_windows() {
  TEST_ASSERT_EQUAL(7 * 60 + 5, parseHHMM(" 07:05 "));
  TEST_ASSERT_EQUAL(-1, parseHHMM("07:0Z"));
  TEST_ASSERT_EQUAL(-1, parseHHMM("24:00"));
  TEST_ASSERT_EQUAL(-1, parseHHMM("7"));
  TEST_ASSERT_TRUE(inWindow(420, 420, 630));
  TEST_ASSERT_FALSE(inWindow(630, 420, 630));      // end is exclusive
  TEST_ASSERT_TRUE(inWindow(23 * 60, 22 * 60, 6 * 60));
  TEST_ASSERT_TRUE(inWindow(5 * 60, 22 * 60, 6 * 60));
  TEST_ASSERT_FALSE(inWindow(12 * 60, 22 * 60, 6 * 60));
  TEST_ASSERT_FALSE(inWindow(600, 600, 600));      // zero length matches nothing
}

void test_layers_and_first_match() {
  const Program def = prog("messages");
  std::vector<Rule> rules = {rule("Breakfast", {0, 1, 2, 3, 4}, "07:00", "10:30", "text"),
                             rule("Mornings", {}, "06:00", "12:00", "clock")};
  Rule xmas;
  xmas.name = "Christmas";
  xmas.date = "2026-12-25";
  xmas.program = prog("weather");
  std::vector<Rule> overrides = {xmas};
  // Thursday 2026-10-01 08:00: Breakfast (first match), not Mornings.
  Choice c = chooseProgram(def, true, rules, overrides, true, at(2026, 10, 1, 3, 8, 0));
  TEST_ASSERT_EQUAL_STRING("text", c.program.source.c_str());
  TEST_ASSERT_EQUAL_STRING("rule: Breakfast", c.reason.c_str());
  // Saturday 08:00: Breakfast is weekdays only -> Mornings.
  c = chooseProgram(def, true, rules, overrides, true, at(2026, 10, 3, 5, 8, 0));
  TEST_ASSERT_EQUAL_STRING("rule: Mornings", c.reason.c_str());
  // Afternoon: the default.
  c = chooseProgram(def, true, rules, overrides, true, at(2026, 10, 3, 5, 15, 0));
  TEST_ASSERT_EQUAL_STRING("default", c.reason.c_str());
  TEST_ASSERT_EQUAL_STRING("messages", c.program.source.c_str());
  // Christmas morning (a Friday): the override outranks Breakfast all day.
  c = chooseProgram(def, true, rules, overrides, true, at(2026, 12, 25, 4, 8, 0));
  TEST_ASSERT_EQUAL_STRING("override: Christmas", c.reason.c_str());
  TEST_ASSERT_EQUAL(2, c.kind);
}

void test_gates_and_broken_rules() {
  const Program def = prog("messages");
  std::vector<Rule> rules = {rule("Typo", {}, "07:0Z", "10:00", "text"),      // broken time: never matches
                             rule("Empty", {}, "", "", ""),                    // no program: skipped
                             rule("All day", {}, "", "", "clock")};
  Choice c = chooseProgram(def, true, rules, {}, true, at(2026, 10, 1, 3, 8, 0));
  TEST_ASSERT_EQUAL_STRING("rule: All day", c.reason.c_str());
  c = chooseProgram(def, false, rules, {}, true, at(2026, 10, 1, 3, 8, 0));
  TEST_ASSERT_EQUAL_STRING("schedule off", c.reason.c_str());
  c = chooseProgram(def, true, rules, {}, false, at(2026, 10, 1, 3, 8, 0));
  TEST_ASSERT_EQUAL_STRING("waiting for the clock", c.reason.c_str());
  TEST_ASSERT_EQUAL_STRING("messages", c.program.source.c_str());
}

void test_midnight_belongs_to_the_start_day() {
  // Fridays 22:00-06:00. Saturday 2026-10-03 02:00 is still Friday night.
  std::vector<Rule> rules = {rule("Friday night", {4}, "22:00", "06:00", "clock")};
  Choice c = chooseProgram(prog("messages"), true, rules, {}, true, at(2026, 10, 3, 5, 2, 0));
  TEST_ASSERT_EQUAL_STRING("rule: Friday night", c.reason.c_str());
  // Friday 02:00 is Thursday night: no match.
  c = chooseProgram(prog("messages"), true, rules, {}, true, at(2026, 10, 2, 4, 2, 0));
  TEST_ASSERT_EQUAL_STRING("default", c.reason.c_str());
  // An override for New Year's Eve 22:00-02:00 still holds at 01:00 on Jan 1.
  Rule nye;
  nye.name = "New Year";
  nye.date = "2026-12-31";
  nye.start = "22:00";
  nye.end = "02:00";
  nye.program = prog("text");
  c = chooseProgram(prog("messages"), true, {}, {nye}, true, at(2027, 1, 1, 4, 1, 0));
  TEST_ASSERT_EQUAL_STRING("override: New Year", c.reason.c_str());
}

void test_power() {
  PowerInput in;
  in.clock_ok = true;
  in.now = at(2026, 10, 1, 3, 23, 30);
  TEST_ASSERT_TRUE(choosePower(in).on);
  in.sleep_enabled = true;
  in.sleep = {rule("Overnight", {}, "23:00", "06:30", "")};
  Power p = choosePower(in);
  TEST_ASSERT_FALSE(p.on);
  TEST_ASSERT_EQUAL_STRING("asleep: Overnight", p.reason.c_str());
  in.woken = true;                       // a tap at night lights it
  TEST_ASSERT_TRUE(choosePower(in).on);
  in.latch_off = true;                   // but "off" from Home Assistant outranks even that
  p = choosePower(in);
  TEST_ASSERT_FALSE(p.on);
  TEST_ASSERT_EQUAL_STRING("turned off remotely", p.reason.c_str());
  in.latch_off = false;
  in.woken = false;
  in.clock_ok = false;                   // clock unknown: fail on, not dark
  TEST_ASSERT_TRUE(choosePower(in).on);
  in.clock_ok = true;
  in.now = at(2026, 10, 1, 3, 12, 0);    // daytime, motion in use and none lately: off
  in.motion = 0;
  p = choosePower(in);
  TEST_ASSERT_FALSE(p.on);
  TEST_ASSERT_EQUAL_STRING("no motion lately", p.reason.c_str());
  in.motion = 1;
  TEST_ASSERT_TRUE(choosePower(in).on);
  in.sleep = {rule("All day by mistake", {}, "", "", "")};   // all-day sleep is ignored
  in.motion = -1;
  TEST_ASSERT_TRUE(choosePower(in).on);
}

void test_describe() {
  TEST_ASSERT_EQUAL_STRING("Mon,Fri 07:00-10:30", describe(rule("", {4, 0}, "07:00", "10:30", "x")).c_str());
  TEST_ASSERT_EQUAL_STRING("every day all day", describe(rule("", {}, "", "", "x")).c_str());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_times_and_windows);
  RUN_TEST(test_layers_and_first_match);
  RUN_TEST(test_gates_and_broken_rules);
  RUN_TEST(test_midnight_belongs_to_the_start_day);
  RUN_TEST(test_power);
  RUN_TEST(test_describe);
  return UNITY_END();
}
