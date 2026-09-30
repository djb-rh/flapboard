#include "config.h"

#include <LittleFS.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cctype>

#include "note.h"

namespace flapboard {
namespace config {
namespace {

constexpr const char *kPath = "/config.json";
constexpr const char *kTmp = "/config.json.tmp";

// Every setting, with its default. Later phases add keys here.
constexpr const char *kDefaults = R"JSON({
  "device_name": "FlapBoard"
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
  File f = LittleFS.open(kTmp, "w");
  if (!f) return false;
  const size_t n = serializeJsonPretty(g_doc, f);
  f.close();
  if (!n) return false;
  LittleFS.remove(kPath);
  return LittleFS.rename(kTmp, kPath);   // never a half-written config
}

}  // namespace

Reader::Reader() { xSemaphoreTake(g_mux, portMAX_DELAY); }
Reader::~Reader() { xSemaphoreGive(g_mux); }
JsonDocument &Reader::doc() { return g_doc; }

void begin() {
  g_mux = xSemaphoreCreateMutex();
  deserializeJson(g_defaults, kDefaults);
  g_doc.set(g_defaults);
  // formatOnFail: a blank or corrupt partition becomes an empty filesystem.
  g_fs = LittleFS.begin(true, "/littlefs", 5, "spiffs");
  if (!g_fs) {
    note("config: LittleFS would not mount; running on defaults");
    return;
  }
  File f = LittleFS.open(kPath, "r");
  if (!f) {
    save();
    note("config: created %s", kPath);
    return;
  }
  JsonDocument file;
  const DeserializationError e = deserializeJson(file, f);
  f.close();
  if (e) {
    note("config: %s is unreadable (%s); using defaults", kPath, e.c_str());
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
