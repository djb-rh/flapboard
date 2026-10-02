#include "power.h"
#include "web.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <algorithm>

#include "clock.h"
#include "config.h"
#include "note.h"
#include "schedule.h"
#include "motion.h"
#include "sign.h"
#include "sound.h"

namespace flapboard {
namespace power {
namespace {

// Pins a relay may use: Port A (G53 is the Grove relay unit's signal wire)
// and the M5-Bus GPIOs from M5Stack's pin map. Everything else on the Tab5
// belongs to the display, camera, audio, SD card or Wi-Fi.
const int kAllowed[] = {53, 54, 2, 3, 4, 16, 17, 18, 45, 47, 48, 51, 52};

bool g_on = true;
bool g_latch = false;
volatile int g_latch_req = -1;      // -1 none, 0 release, 1 off
volatile uint32_t g_relay_test_until = 0;
uint32_t g_wake_until = 0;
int g_pin = -1;
bool g_active_high = true;
bool g_relay = false;
int g_brightness = -1;
volatile bool g_blanked_for_update = false;
std::string g_reason = "starting";
int g_ext5v = -1;   // Port A / M5-Bus 5 V rail: -1 unknown, 0 off, 1 on

bool allowed(int pin) {
  for (int p : kAllowed)
    if (p == pin) return true;
  return false;
}

void readRelayConfig(int *pin, bool *high) {
  config::Reader r;
  *pin = r.doc()["relay_pin"] | -1;
  *high = r.doc()["relay_active_high"] | true;
}

void driveRelay(bool on) {
  if (g_pin < 0) return;
  digitalWrite(g_pin, (on == g_active_high) ? HIGH : LOW);
  g_relay = on;
}

void setupPin() {
  int pin;
  bool high;
  readRelayConfig(&pin, &high);
  if (pin == g_pin && high == g_active_high) return;
  if (g_pin >= 0) pinMode(g_pin, INPUT);   // release the old pin
  g_pin = -1;
  g_relay = false;
  g_active_high = high;
  if (pin >= 0 && !allowed(pin)) {
    note("power: GPIO %d is not free on the Tab5; relay disabled", pin);
    return;
  }
  if (pin < 0) return;
  g_pin = pin;
  digitalWrite(g_pin, high ? LOW : HIGH);   // the off level before it becomes an output
  pinMode(g_pin, OUTPUT);
  driveRelay(false);
}

std::vector<schedule::Rule> sleepRules(bool *enabled) {
  std::vector<schedule::Rule> out;
  config::Reader r;
  *enabled = r.doc()["sleep_enabled"] | false;
  for (JsonVariantConst v : r.doc()["sleep_rules"].as<JsonArrayConst>()) {
    schedule::Rule x;
    x.name = v["name"] | "";
    x.start = v["start"] | "";
    x.end = v["end"] | "";
    for (JsonVariantConst d : v["days"].as<JsonArrayConst>()) x.days.push_back(d.as<int>());
    out.push_back(x);
  }
  return out;
}

schedule::Now nowLocal() {
  schedule::Now n;
  struct tm t;
  if (clock::localNow(&t)) {
    n.year = t.tm_year + 1900;
    n.month = t.tm_mon + 1;
    n.day = t.tm_mday;
    n.weekday = (t.tm_wday + 6) % 7;   // Monday = 0
    n.minutes = t.tm_hour * 60 + t.tm_min;
  }
  return n;
}

void apply(bool on, int brightness) {
  if (on) {
    M5.Display.setBrightness(brightness);
    sign::setActive(true);   // redraws the board
  } else {
    sign::setActive(false);
    M5.Display.setBrightness(0);
  }
  sound::setSuppressed(!on);
}

}  // namespace

void beginEarly() {
  Preferences p;
  p.begin("flapboard", true);
  g_latch = p.getBool("latch_off", false);
  p.end();
  setupPin();   // before anything else can glitch the pin: the lights stay off until decided
}

void begin() {}

std::string buildStatus();
extern SemaphoreHandle_t g_status_mux;
extern std::string g_status_json;

void loop() {
  static uint32_t last = 0;
  if (millis() - last < 250) return;
  last = millis();

  if (g_latch_req >= 0) {   // flash write: here, on the main loop
    g_latch = g_latch_req == 1;
    g_latch_req = -1;
    Preferences p;
    p.begin("flapboard", false);
    p.putBool("latch_off", g_latch);
    p.end();
    note("power: %s", g_latch ? "turned off (held until turned on again)" : "off-hold released");
  }
  setupPin();
  // A relay module needs power: Grove Port A's red wire (and the M5-Bus 5 V
  // pin) is a switched rail, off at boot (output_power = false). On while a
  // relay is configured. This is an IO-expander write, so it lives here on
  // the main loop.
  const int want5v = g_pin >= 0 ? 1 : 0;
  if (want5v != g_ext5v) {
    M5.Power.setExtOutput(want5v == 1, m5::ext_PA);
    g_ext5v = want5v;
    note("power: Port A 5 V %s", want5v ? "on (for the relay)" : "off");
  }

  int brightness, wake_min, motion_min;
  {
    config::Reader r;
    brightness = (int)((r.doc()["brightness"] | 80) * 255 / 100);
    wake_min = r.doc()["tap_wake_minutes"] | 5;
    motion_min = r.doc()["motion_timeout"] | 10;
  }
  if (brightness < 8) brightness = 8;
  schedule::PowerInput in;
  in.latch_off = g_latch;
  in.sleep = sleepRules(&in.sleep_enabled);
  in.clock_ok = clock::trusted();
  in.now = nowLocal();
  in.woken = g_wake_until && (int32_t)(millis() - g_wake_until) < 0;
  in.motion = motion::state((uint32_t)std::max(1, motion_min) * 60000);
  (void)wake_min;
  const schedule::Power p = schedule::choosePower(in);
  // During a firmware update the screen stays dark (see web::updating);
  // afterwards g_brightness = -1 makes the block below light it again.
  static bool updating = false;
  if (web::updating() != updating) {
    updating = !updating;
    if (updating) apply(false, 0);
    else g_brightness = -1;
    g_blanked_for_update = updating;
  }
  if (!updating && (p.on != g_on || p.reason != g_reason || (p.on && brightness != g_brightness))) {
    if (p.on != g_on) note("power: %s (%s)", p.on ? "on" : "off", p.reason.c_str());
    if (p.on != g_on || brightness != g_brightness) apply(p.on, brightness);
    if (p.on != g_on) motion::powerChanged();   // the scene's lighting just changed
    g_on = p.on;
    g_reason = p.reason;
    g_brightness = brightness;
  }
  const bool testing = g_relay_test_until && (int32_t)(millis() - g_relay_test_until) < 0;
  if (!testing) g_relay_test_until = 0;
  const bool relay = g_on || testing;
  if (g_pin >= 0 && relay != g_relay) driveRelay(relay);
  std::string st = buildStatus();
  xSemaphoreTake(g_status_mux, portMAX_DELAY);
  g_status_json.swap(st);
  xSemaphoreGive(g_status_mux);
}

bool isOn() { return g_on; }
bool blankedForUpdate() { return g_blanked_for_update; }

void wake() {
  int minutes;
  {
    config::Reader r;
    minutes = r.doc()["tap_wake_minutes"] | 5;
  }
  if (minutes <= 0) return;
  g_wake_until = millis() + (uint32_t)minutes * 60000;
  if (!g_wake_until) g_wake_until = 1;
}

void requestLatch(bool off) { g_latch_req = off ? 1 : 0; }
void testRelay(int seconds) { g_relay_test_until = millis() + (uint32_t)seconds * 1000; }

SemaphoreHandle_t g_status_mux = xSemaphoreCreateMutex();
std::string g_status_json = "{}";

std::string buildStatus() {
  JsonDocument d;
  d["on"] = g_on;
  d["reason"] = g_reason;
  d["held_off"] = g_latch;
  d["relay_pin"] = g_pin;
  d["relay_on"] = g_pin >= 0 && g_relay;
  d["port_a_5v"] = g_ext5v == 1;
  d["woken_for_s"] = g_wake_until && (int32_t)(millis() - g_wake_until) < 0 ? (g_wake_until - millis()) / 1000 : 0;
  std::string out;
  serializeJson(d, out);
  return out;
}

// Built by the main loop (which owns g_reason); read by any task.
std::string statusJson() {
  xSemaphoreTake(g_status_mux, portMAX_DELAY);
  std::string s = g_status_json;
  xSemaphoreGive(g_status_mux);
  return s;
}

}  // namespace power
}  // namespace flapboard
