#include "config.h"

#include <Preferences.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cctype>

#include "note.h"

namespace flapboard {
namespace config {
namespace {

// The settings JSON lives in its own NVS partition ("cfg", 256 KB): no
// filesystem, so no VFS slot (see partitions.csv), and NVS writes of a key
// are atomic, so a power cut never leaves half a file.
constexpr const char *kPart = "cfg";
constexpr const char *kNs = "settings";
constexpr const char *kKey = "json";

// Every setting, with its default. Later phases add keys here.
constexpr const char *kDefaults = R"JSON({
  "device_name": "FlapBoard",
  "sound_enabled": true,
  "sound_volume": 60,
  "sound_offset_ms": 0,
  "board_rows": 6,
  "board_cols": 22,
  "flap_width": 0,
  "flap_aspect": 1.4,
  "flap_gap": 4,
  "board_margin": 12,
  "glyph_size": 62,
  "theme": "solari",
  "color_background": "",
  "color_flap": "",
  "color_glyph": "",
  "font": "BebasNeue-Regular",
  "side_left": "",
  "side_right": "",
  "side_width": 15,
  "side_fit": "contain",
  "side_background": "#000000",
  "orientation": "landscape",
  "flip_ms": 70,
  "speed_variance": 3,
  "start_mode": "random",
  "content_source": "messages",
  "content_files": [],
  "content_order": "random",
  "content_dwell": 20,
  "content_text": "",
  "clock_template": "{time}|{date}",
  "clock_rb_mode": false,
  "clock_friday_text": "HAPPY FRIDAY!",
  "weather_template": "{place}|NOW {temp}\u00B0 {cond}|HI {hi}  LO {lo}",
  "time_format": "%-I:%M %p",
  "date_format": "%a %b %-d",
  "timezone": "America/New_York",
  "ntp_server": "pool.ntp.org",
  "weather_lat": 0.0,
  "weather_lon": 0.0,
  "weather_place": "",
  "units": "imperial",
  "weather_minutes": 15,
  "schedule_enabled": false,
  "schedule_rules": [],
  "schedule_overrides": [],
  "sleep_enabled": false,
  "sleep_rules": [],
  "brightness": 80,
  "tap_wake_minutes": 5,
  "relay_pin": -1,
  "relay_active_high": true,
  "mqtt_host": "",
  "mqtt_port": 1883,
  "mqtt_user": "",
  "motion_enabled": false,
  "motion_timeout": 10,
  "motion_threshold": 14,
  "motion_area": 1.5,
  "photo_selection": [],
  "photo_order": "random",
  "photo_dwell": 30,
  "photo_transition": "dissolve",
  "photo_transition_ms": 700,
  "photo_fit": "contain",
  "photo_blur": true,
  "photo_clock": false,
  "photo_clock_pos": "bottom_right"
})JSON";

JsonDocument g_doc;
JsonDocument g_defaults;
SemaphoreHandle_t g_mux = nullptr;
bool g_fs = false;

bool compatible(JsonVariantConst def, JsonVariantConst v) {
  if (def.is<bool>()) return v.is<bool>();
  if (def.is<const char *>()) return v.is<const char *>();
  if (def.is<JsonArrayConst>()) return v.is<JsonArrayConst>();
  if (def.is<JsonObjectConst>()) return v.is<JsonObjectConst>();
  if (def.is<float>() || def.is<long>()) return v.is<float>() || v.is<long>();
  return false;
}

bool save() {
  if (!g_fs) return false;
  std::string out;
  serializeJson(g_doc, out);
  Preferences p;
  if (!p.begin(kNs, false, kPart)) return false;
  const bool ok = p.putBytes(kKey, out.data(), out.size()) == out.size();
  p.end();
  return ok;
}

}  // namespace

Reader::Reader() { xSemaphoreTake(g_mux, portMAX_DELAY); }
Reader::~Reader() { xSemaphoreGive(g_mux); }
JsonDocument &Reader::doc() { return g_doc; }

void begin() {
  g_mux = xSemaphoreCreateMutex();
  deserializeJson(g_defaults, kDefaults);
  g_doc.set(g_defaults);
  esp_err_t e0 = nvs_flash_init_partition(kPart);
  if (e0 == ESP_ERR_NVS_NO_FREE_PAGES || e0 == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase_partition(kPart);   // a blank or foreign partition: start it fresh
    e0 = nvs_flash_init_partition(kPart);
  }
  g_fs = e0 == ESP_OK;
  if (!g_fs) {
    note("config: settings partition unavailable (%s); running on defaults", esp_err_to_name(e0));
    return;
  }
  std::string text;
  {
    Preferences p;
    if (p.begin(kNs, true, kPart)) {
      const size_t n = p.getBytesLength(kKey);
      if (n) {
        text.assign(n, '\0');
        p.getBytes(kKey, &text[0], n);
      }
      p.end();
    }
  }
  if (text.empty()) {
    save();
    note("config: settings created");
    return;
  }
  JsonDocument file;
  const DeserializationError e = deserializeJson(file, text);
  if (e) {
    note("config: stored settings are unreadable (%s); using defaults", e.c_str());
    return;
  }
  for (JsonPairConst kv : file.as<JsonObjectConst>()) {
    JsonVariantConst def = g_defaults[kv.key()];
    if (!def.isNull() && compatible(def, kv.value())) g_doc[kv.key()] = kv.value();
  }
}

std::string toJson() {
  Reader r;
  std::string out;
  serializeJson(g_doc, out);
  return out;
}

bool apply(JsonVariantConst patch, std::string *error) {
  if (!patch.is<JsonObjectConst>()) {
    *error = "expected a JSON object";
    return false;
  }
  std::string refused;
  Reader r;
  for (JsonPairConst kv : patch.as<JsonObjectConst>()) {
    JsonVariantConst def = g_defaults[kv.key()];
    if (def.isNull() || !compatible(def, kv.value())) {
      refused += (refused.empty() ? "" : ", ") + std::string(kv.key().c_str());
    }
  }
  if (!refused.empty()) {
    *error = "unknown setting or wrong type: " + refused;
    return false;
  }
  for (JsonPairConst kv : patch.as<JsonObjectConst>()) g_doc[kv.key()] = kv.value();
  if (!save()) {
    *error = "could not write the settings file";
    return false;
  }
  return true;
}

int applyKnown(JsonVariantConst patch) {
  int n = 0;
  {
    Reader r;
    for (JsonPairConst kv : patch.as<JsonObjectConst>()) {
      JsonVariantConst def = g_defaults[kv.key()];
      if (def.isNull() || !compatible(def, kv.value())) continue;
      g_doc[kv.key()] = kv.value();
      n++;
    }
    if (n) save();
  }
  return n;
}

std::string deviceName() {
  Reader r;
  std::string n = g_doc["device_name"] | "FlapBoard";
  return n.empty() ? "FlapBoard" : n;
}

std::string slugify(const std::string &name) {
  std::string out;
  bool dash = false;
  for (unsigned char c : name) {
    if (std::isalnum(c)) {
      if (dash && !out.empty()) out += '-';
      out += (char)std::tolower(c);
      dash = false;
    } else {
      dash = true;
    }
    if (out.size() >= 63) break;
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  return out.empty() ? "flapboard" : out;
}

std::string hostname() { return slugify(deviceName()); }

namespace secrets {

std::string get(const char *key) {
  Preferences p;
  if (!p.begin("flapboard", true)) return "";
  std::string v = p.getString(key, "").c_str();
  p.end();
  return v;
}

std::string wifiSsid() { return get("ssid"); }
std::string wifiPass() { return get("pass"); }

void setWifi(const std::string &ssid, const std::string &pass) {
  Preferences p;
  p.begin("flapboard", false);
  p.putString("ssid", ssid.c_str());
  p.putString("pass", pass.c_str());
  p.end();
}

}  // namespace secrets
}  // namespace config
}  // namespace flapboard
