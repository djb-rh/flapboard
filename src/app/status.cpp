#include "status.h"

#include <ArduinoJson.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "config.h"
#include "content.h"
#include "motion.h"
#include "mqtt.h"
#include "power.h"
#include "weather.h"
#include "net.h"
#include "sdcard.h"
#include "sign.h"
#include "sound.h"

namespace flapboard {
namespace status {
namespace {

SemaphoreHandle_t g_mux = xSemaphoreCreateMutex();
std::string g_rtc = "";
int g_batt_mv = 0, g_batt_pct = 0, g_batt_ma = 0;
uint32_t g_last = 0;

}  // namespace

void update() {
  if (g_last && millis() - g_last < 2000) return;
  g_last = millis();
  auto dt = M5.Rtc.getDateTime();
  char t[32];
  snprintf(t, sizeof(t), "%04d-%02d-%02d %02d:%02d:%02d UTC", dt.date.year, dt.date.month, dt.date.date,
           dt.time.hours, dt.time.minutes, dt.time.seconds);
  const int mv = M5.Power.getBatteryVoltage(), pct = M5.Power.getBatteryLevel();
  const int ma = (int)M5.Power.getBatteryCurrent();
  xSemaphoreTake(g_mux, portMAX_DELAY);
  g_rtc = t;
  g_batt_mv = mv;
  g_batt_pct = pct;
  g_batt_ma = ma;
  xSemaphoreGive(g_mux);
}

std::string rtcText() {
  xSemaphoreTake(g_mux, portMAX_DELAY);
  std::string r = g_rtc;
  xSemaphoreGive(g_mux);
  return r;
}

std::string json() {
  JsonDocument d;
  d["version"] = FLAPBOARD_VERSION;
  if (const esp_partition_t *run = esp_ota_get_running_partition()) {
    d["app_slot"] = run->label;
    esp_ota_img_states_t st;
    // "trial": a new firmware not yet confirmed healthy (rolls back if it restarts now).
    d["app_trial"] = esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
  }
  d["device_name"] = config::deviceName();
  d["hostname"] = config::hostname() + ".local";
  d["uptime_s"] = millis() / 1000;
  JsonObject w = d["wifi"].to<JsonObject>();
  const net::State st = net::state();
  w["state"] = st == net::State::Connected ? "connected" : st == net::State::Connecting ? "joining"
               : st == net::State::Failed ? "failed" : "off";
  w["ssid"] = net::ssid();
  w["ip"] = net::ip();
  w["rssi"] = net::rssi();
  w["mac"] = WiFi.macAddress();
  w["access_point"] = net::bssid();
  w["ipv6_link_local"] = WiFi.linkLocalIPv6().toString();
  w["portal"] = net::portalActive();
  w["portal_ssid"] = net::portalSsid();
  const net::Watch wc = net::watch();
  w["pings_ok"] = wc.pings_ok;
  w["pings_lost"] = wc.pings_lost;
  w["rejoins"] = wc.rejoins;
  w["watchdog_restarts"] = wc.restarts;
  JsonObject c6 = d["wifi_chip"].to<JsonObject>();
  c6["firmware"] = net::coprocVersion();
  c6["expected"] = net::hostVersion();
  c6["update_available"] = net::coprocUpdateAvailable();
  JsonObject m = d["memory"].to<JsonObject>();
  m["internal_free"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  m["dma_largest"] = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  m["psram_free"] = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  JsonObject sd = d["sd"].to<JsonObject>();
  sd["mounted"] = sdcard::mounted();
  uint64_t fb = 0, tb = 0;
  sdcard::space(&fb, &tb);
  sd["free_bytes"] = fb;
  sd["total_bytes"] = tb;
  sd["bus"] = sdcard::busWidth();
  sd["problem"] = sdcard::problem();
  xSemaphoreTake(g_mux, portMAX_DELAY);
  d["rtc"] = g_rtc;
  JsonObject b = d["battery"].to<JsonObject>();
  b["mv"] = g_batt_mv;
  b["percent"] = g_batt_pct;
  b["ma"] = g_batt_ma;
  xSemaphoreGive(g_mux);
  d["reset_reason"] = (int)esp_reset_reason();
  JsonDocument sg;
  deserializeJson(sg, sign::statsJson());
  d["sign"] = sg;
  JsonDocument sd2;
  deserializeJson(sd2, sound::statsJson());
  d["sound"] = sd2;
  d["sound"]["volume"] = sound::volume();
  d["sound"]["enabled"] = sound::enabled();
  JsonDocument ct, wx;
  deserializeJson(ct, content::statusJson());
  deserializeJson(wx, weather::statusJson());
  d["content"] = ct;
  JsonDocument pw;
  deserializeJson(pw, power::statusJson());
  d["power"] = pw;
  JsonDocument mq, mo;
  deserializeJson(mq, mqtt::statusJson());
  deserializeJson(mo, motion::statusJson());
  d["mqtt"] = mq;
  d["motion"] = mo;
  d["weather"] = wx;
  std::string out;
  serializeJson(d, out);
  return out;
}

}  // namespace status
}  // namespace flapboard
