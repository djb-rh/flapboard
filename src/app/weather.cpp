#include "weather.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <algorithm>
#include <cmath>

#include "config.h"
#include "net.h"
#include "note.h"

namespace flapboard {
namespace weather {
namespace {

struct Reading {
  bool ok = false;
  float temp = 0, feels = 0, hi = 0, lo = 0, hum = 0, wind = 0;
  int code = 0;
  bool day = true;
  bool metric = false;
  std::string place;
  uint32_t at_ms = 0;
};

SemaphoreHandle_t g_mux;
Reading g_now;
std::string g_error = "not fetched yet";
volatile bool g_refresh = false;
uint32_t g_internal_low = 0;   // least internal RAM seen during a fetch

// WMO weather codes as words short enough for a board.
const char *describe(int code, bool day) {
  switch (code) {
    case 0: return day ? "SUNNY" : "CLEAR";
    case 1: return day ? "MOSTLY SUNNY" : "MOSTLY CLEAR";
    case 2: return "PARTLY CLOUDY";
    case 3: return "CLOUDY";
    case 45: case 48: return "FOG";
    case 51: case 53: case 55: return "DRIZZLE";
    case 56: case 57: return "FREEZING DRIZZLE";
    case 61: return "LIGHT RAIN";
    case 63: return "RAIN";
    case 65: return "HEAVY RAIN";
    case 66: case 67: return "FREEZING RAIN";
    case 71: return "LIGHT SNOW";
    case 73: return "SNOW";
    case 75: return "HEAVY SNOW";
    case 77: return "SNOW GRAINS";
    case 80: case 81: return "SHOWERS";
    case 82: return "HEAVY SHOWERS";
    case 85: case 86: return "SNOW SHOWERS";
    case 95: return "STORMS";
    case 96: case 99: return "STORMS AND HAIL";
  }
  return "";
}

struct Body {
  std::string data;
};

esp_err_t onEvent(esp_http_client_event_t *e) {
  if (e->event_id == HTTP_EVENT_ON_DATA && e->user_data && e->data_len > 0) {
    auto *b = (Body *)e->user_data;
    if (b->data.size() < 16384) b->data.append((const char *)e->data, e->data_len);
  }
  const uint32_t free_now = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  if (!g_internal_low || free_now < g_internal_low) g_internal_low = free_now;
  return ESP_OK;
}

bool fetch(float lat, float lon, bool metric, const std::string &place, std::string *err) {
  char url[400];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m,is_day"
           "&daily=temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=1%s",
           lat, lon, metric ? "" : "&temperature_unit=fahrenheit&wind_speed_unit=mph");
  Body body;
  esp_http_client_config_t c = {};
  c.url = url;
  c.crt_bundle_attach = esp_crt_bundle_attach;
  c.timeout_ms = 10000;
  c.event_handler = onEvent;
  c.user_data = &body;
  c.buffer_size = 2048;
  esp_http_client_handle_t h = esp_http_client_init(&c);
  if (!h) {
    *err = "could not start the request";
    return false;
  }
  const esp_err_t e = esp_http_client_perform(h);
  const int status = esp_http_client_get_status_code(h);
  esp_http_client_cleanup(h);
  if (e != ESP_OK) {
    *err = std::string("request failed: ") + esp_err_to_name(e);
    return false;
  }
  if (status != 200) {
    *err = "Open-Meteo answered HTTP " + std::to_string(status);
    return false;
  }
  JsonDocument d;
  if (deserializeJson(d, body.data)) {
    *err = "unreadable answer";
    return false;
  }
  Reading r;
  r.ok = true;
  r.temp = d["current"]["temperature_2m"] | 0.0f;
  r.feels = d["current"]["apparent_temperature"] | 0.0f;
  r.hum = d["current"]["relative_humidity_2m"] | 0.0f;
  r.wind = d["current"]["wind_speed_10m"] | 0.0f;
  r.code = d["current"]["weather_code"] | 0;
  r.day = (d["current"]["is_day"] | 1) == 1;
  r.hi = d["daily"]["temperature_2m_max"][0] | 0.0f;
  r.lo = d["daily"]["temperature_2m_min"][0] | 0.0f;
  r.metric = metric;
  r.place = place;
  r.at_ms = millis();
  xSemaphoreTake(g_mux, portMAX_DELAY);
  g_now = r;
  xSemaphoreGive(g_mux);
  return true;
}

void task(void *) {
  uint32_t next = 0;
  float last_lat = 0, last_lon = 0;
  bool last_metric = false;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    float lat, lon;
    bool metric;
    int minutes;
    std::string place;
    {
      config::Reader r;
      lat = r.doc()["weather_lat"] | 0.0f;
      lon = r.doc()["weather_lon"] | 0.0f;
      metric = std::string(r.doc()["units"] | "imperial") == "metric";
      minutes = r.doc()["weather_minutes"] | 15;
      place = r.doc()["weather_place"] | "";
    }
    if (lat == 0 && lon == 0) continue;   // no location set
    const bool changed = lat != last_lat || lon != last_lon || metric != last_metric;
    if (!g_refresh && !changed && (int32_t)(millis() - next) < 0) continue;
    if (net::state() != net::State::Connected) continue;
    g_refresh = false;
    last_lat = lat;
    last_lon = lon;
    last_metric = metric;
    std::string err;
    g_internal_low = 0;
    const bool ok = fetch(lat, lon, metric, place, &err);
    if (ok) {
      g_error.clear();
      note("weather: %s %.0f%s, %s (internal RAM low point %u KB)", place.c_str(), g_now.temp, metric ? "C" : "F",
           describe(g_now.code, g_now.day), (unsigned)(g_internal_low / 1024));
      next = millis() + (uint32_t)std::max(5, minutes) * 60000;
    } else {
      g_error = err;
      note("weather: %s; retrying in 2 min", err.c_str());
      next = millis() + 120000;
    }
  }
}

std::string fmt(float v) { return std::to_string((int)std::lround(v)); }

}  // namespace

void begin() {
  g_mux = xSemaphoreCreateMutex();
  // Its stack in PSRAM (internal RAM is what TLS and Wi-Fi need); it never writes flash.
  xTaskCreatePinnedToCoreWithCaps(task, "weather", 8192, nullptr, 2, nullptr, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void refreshNow() { g_refresh = true; }

bool field(const std::string &name, std::string *out) {
  xSemaphoreTake(g_mux, portMAX_DELAY);
  const Reading r = g_now;
  xSemaphoreGive(g_mux);
  const bool known = name == "temp" || name == "feels" || name == "hi" || name == "lo" || name == "hum" ||
                     name == "wind" || name == "cond" || name == "place" || name == "units" || name == "wind_units";
  if (!known) return false;
  if (!r.ok) {
    *out = name == "cond" ? "NO WEATHER YET" : "--";
    return true;
  }
  if (name == "temp") *out = fmt(r.temp);
  else if (name == "feels") *out = fmt(r.feels);
  else if (name == "hi") *out = fmt(r.hi);
  else if (name == "lo") *out = fmt(r.lo);
  else if (name == "hum") *out = fmt(r.hum);
  else if (name == "wind") *out = fmt(r.wind);
  else if (name == "cond") *out = describe(r.code, r.day);
  else if (name == "place") *out = r.place;
  else if (name == "units") *out = r.metric ? "C" : "F";
  else *out = r.metric ? "KMH" : "MPH";
  return true;
}

std::string statusJson() {
  xSemaphoreTake(g_mux, portMAX_DELAY);
  const Reading r = g_now;
  const std::string err = g_error;
  xSemaphoreGive(g_mux);
  JsonDocument d;
  d["ok"] = r.ok;
  if (r.ok) {
    d["temp"] = r.temp;
    d["hi"] = r.hi;
    d["lo"] = r.lo;
    d["cond"] = describe(r.code, r.day);
    d["age_s"] = (millis() - r.at_ms) / 1000;
    d["place"] = r.place;
  }
  d["error"] = err;
  std::string out;
  serializeJson(d, out);
  return out;
}

}  // namespace weather
}  // namespace flapboard
