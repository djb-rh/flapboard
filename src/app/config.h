// Settings: one JSON document in the "cfg" NVS partition, merged over built-in defaults on load (as
// the Pi frame does), so a firmware that adds a key upgrades old files
// unchanged. Secrets (Wi-Fi and broker passwords) live in NVS instead and
// are never sent back to a browser.
//
// The config lives in flash, not on the card, so pulling the SD card does
// not wipe the sign.
#pragma once

#include <ArduinoJson.h>

#include <string>

namespace flapboard {
namespace config {

void begin();   // loads (or creates) the settings

// Read access: call with the lock held via Reader, e.g.
//   { config::Reader r; name = r.doc()["device_name"].as<std::string>(); }
class Reader {
 public:
  Reader();
  ~Reader();
  JsonDocument &doc();
};

std::string toJson();
// Applies the keys in `patch` that exist in the defaults with a compatible
// type; anything else is refused and named in *error. Saves on success.
bool apply(JsonVariantConst patch, std::string *error);
// For restoring a backup: applies the keys this firmware knows, skips the
// rest (a backup from an older or newer version), returns how many applied.
int applyKnown(JsonVariantConst patch);

// Convenience getters.
std::string deviceName();
std::string hostname();   // device name as a DNS label: "Kitchen Sign" -> "kitchen-sign"
std::string slugify(const std::string &name);

// NVS-backed secrets.
namespace secrets {
std::string wifiSsid();
std::string wifiPass();
void setWifi(const std::string &ssid, const std::string &pass);
}  // namespace secrets

}  // namespace config
}  // namespace flapboard
