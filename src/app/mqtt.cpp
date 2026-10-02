#include "mqtt.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <mqtt_client.h>

#include <cstring>

#include "config.h"
#include "content.h"
#include "motion.h"
#include "net.h"
#include "note.h"
#include "power.h"
#include "sound.h"

namespace flapboard {
namespace mqtt {
namespace {

esp_mqtt_client_handle_t g_client = nullptr;
volatile bool g_connected = false;
volatile bool g_need_discovery = false;
std::string g_uid, g_base, g_server_key, g_error = "not configured";
std::string g_will;   // must outlive the client config
// Whether a password is stored, cached: reading NVS stalls the flash cache,
// and statusJson() is also called from the render task (PSRAM stack), where
// that asserts and resets the board.
volatile bool g_has_password = false;
uint32_t g_last_heartbeat = 0;
std::string g_last_state;   // what was published last (publish on change)

struct Command {
  char topic[24];
  char payload[512];
};
QueueHandle_t g_cmds;

const char *const kShowNames[] = {"Messages", "Clock", "Weather", "Fixed message"};
const char *const kShowSources[] = {"messages", "clock", "weather", "text"};

std::string uid() {
  const uint64_t mac = ESP.getEfuseMac();
  char b[16];
  snprintf(b, sizeof(b), "%02x%02x%02x", (unsigned)((mac >> 24) & 0xFF), (unsigned)((mac >> 32) & 0xFF),
           (unsigned)((mac >> 40) & 0xFF));
  return b;
}

std::string password() {
  Preferences p;
  if (!p.begin("flapboard", true)) return "";
  std::string v = p.getString("mqtt_pass", "").c_str();
  p.end();
  return v;
}

void publish(const std::string &topic, const std::string &payload, bool retain = true) {
  if (g_client && g_connected) esp_mqtt_client_publish(g_client, topic.c_str(), payload.c_str(), (int)payload.size(), 1, retain);
}

// One discovery config: homeassistant/<component>/<uid>_<key>/config.
void discover(const char *component, const char *key, JsonDocument &d) {
  d["~"] = g_base;
  d["unique_id"] = "flapboard_" + g_uid + "_" + key;
  d["availability_topic"] = "~/status";
  JsonObject dev = d["device"].to<JsonObject>();
  dev["identifiers"][0] = "flapboard_" + g_uid;
  dev["name"] = config::deviceName();
  dev["manufacturer"] = "FlapBoard";
  dev["model"] = "M5Stack Tab5 split-flap sign";
  dev["sw_version"] = FLAPBOARD_VERSION;
  dev["configuration_url"] = "http://" + net::ip() + "/";
  std::string out;
  serializeJson(d, out);
  publish(std::string("homeassistant/") + component + "/" + g_uid + "_" + key + "/config", out);
}

void sendDiscovery() {
  {
    JsonDocument d;   // the display (and the car's lights): name null = the device's own name
    d["name"] = nullptr;
    d["icon"] = "mdi:train-car";
    d["state_topic"] = "~/display/state";
    d["command_topic"] = "~/display/set";
    d["payload_on"] = "ON";
    d["payload_off"] = "OFF";
    discover("light", "display", d);
  }
  {
    JsonDocument d;
    d["name"] = "Message";
    d["icon"] = "mdi:message-text";
    d["command_topic"] = "~/message/set";
    d["state_topic"] = "~/message/state";
    d["max"] = 255;
    discover("text", "message", d);
  }
  {
    JsonDocument d;
    d["name"] = "Show";
    d["icon"] = "mdi:view-dashboard";
    d["command_topic"] = "~/show/set";
    d["state_topic"] = "~/show/state";
    for (const char *o : kShowNames) d["options"].add(o);
    discover("select", "show", d);
  }
  {
    JsonDocument d;
    d["name"] = "Volume";
    d["icon"] = "mdi:volume-high";
    d["command_topic"] = "~/volume/set";
    d["state_topic"] = "~/volume/state";
    d["min"] = 0;
    d["max"] = 100;
    d["step"] = 5;
    d["unit_of_measurement"] = "%";
    discover("number", "volume", d);
  }
  {
    JsonDocument d;
    d["name"] = "Brightness";
    d["icon"] = "mdi:brightness-6";
    d["command_topic"] = "~/brightness/set";
    d["state_topic"] = "~/brightness/state";
    d["min"] = 5;
    d["max"] = 100;
    d["step"] = 5;
    d["unit_of_measurement"] = "%";
    discover("number", "brightness", d);
  }
  {
    JsonDocument d;
    d["name"] = "Sound";
    d["icon"] = "mdi:volume-source";
    d["command_topic"] = "~/sound/set";
    d["state_topic"] = "~/sound/state";
    discover("switch", "sound", d);
  }
  {
    JsonDocument d;
    d["name"] = "Next message";
    d["icon"] = "mdi:skip-next";
    d["command_topic"] = "~/next/press";
    discover("button", "next", d);
  }
  {
    JsonDocument d;
    d["name"] = "Motion";
    d["device_class"] = "motion";
    d["state_topic"] = "~/motion/state";
    discover("binary_sensor", "motion", d);
  }
  struct Diag {
    const char *key, *name, *tpl, *cls, *unit, *icon;
  } diags[] = {
      {"ip", "IP address", "{{ value_json.ip }}", nullptr, nullptr, "mdi:ip-network"},
      {"rssi", "Wi-Fi signal", "{{ value_json.rssi }}", "signal_strength", "dBm", nullptr},
      {"uptime", "Uptime", "{{ value_json.uptime }}", "duration", "s", nullptr},
      {"showing", "Showing because", "{{ value_json.showing }}", nullptr, nullptr, "mdi:calendar-clock"},
      {"power", "Display state", "{{ value_json.power }}", nullptr, nullptr, "mdi:power"},
      {"heap", "Free internal memory", "{{ value_json.heap_kb }}", "data_size", "kB", nullptr},
      {"wifi_fw", "Wi-Fi chip firmware", "{{ value_json.wifi_fw }}", nullptr, nullptr, "mdi:chip"},
  };
  for (auto &x : diags) {
    JsonDocument d;
    d["name"] = x.name;
    d["state_topic"] = "~/diag";
    d["value_template"] = x.tpl;
    d["entity_category"] = "diagnostic";
    if (x.cls) d["device_class"] = x.cls;
    if (x.unit) d["unit_of_measurement"] = x.unit;
    if (x.icon) d["icon"] = x.icon;
    discover("sensor", x.key, d);
  }
}

void onEvent(void *, esp_event_base_t, int32_t id, void *data) {
  auto *e = (esp_mqtt_event_handle_t)data;
  switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
      g_connected = true;
      g_error.clear();
      const char *subs[] = {"display/set", "message/set", "show/set", "volume/set",
                            "brightness/set", "sound/set", "next/press"};
      for (const char *s : subs) esp_mqtt_client_subscribe(g_client, (g_base + "/" + s).c_str(), 1);
      g_need_discovery = true;   // published from the main loop (it reads settings)
      break;
    }
    case MQTT_EVENT_DISCONNECTED:
      g_connected = false;
      break;
    case MQTT_EVENT_ERROR:
      if (e && e->error_handle) {
        char b[96];
        snprintf(b, sizeof(b), "connection error (tls %d, sock errno %d, refused %d)", e->error_handle->esp_tls_last_esp_err,
                 e->error_handle->esp_transport_sock_errno, e->error_handle->connect_return_code);
        g_error = e->error_handle->connect_return_code == MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
                          e->error_handle->connect_return_code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED
                      ? "the broker refused the user name or password"
                      : b;
      }
      break;
    case MQTT_EVENT_DATA: {
      if (!e->topic || e->topic_len <= (int)g_base.size() + 1) break;
      Command c = {};
      const int tl = std::min<int>(e->topic_len - (int)g_base.size() - 1, sizeof(c.topic) - 1);
      memcpy(c.topic, e->topic + g_base.size() + 1, tl);
      const int pl = std::min<int>(e->data_len, sizeof(c.payload) - 1);
      memcpy(c.payload, e->data, pl);
      xQueueSend(g_cmds, &c, 0);
      break;
    }
    default:
      break;
  }
}

void connect() {
  std::string host, user;
  int port;
  {
    config::Reader r;
    host = r.doc()["mqtt_host"] | "";
    port = r.doc()["mqtt_port"] | 1883;
    user = r.doc()["mqtt_user"] | "";
  }
  const std::string pass = password();
  g_has_password = !pass.empty();
  const std::string key = host + ":" + std::to_string(port) + ":" + user + ":" + pass;
  if (key == g_server_key) return;
  g_server_key = key;
  if (g_client) {
    esp_mqtt_client_stop(g_client);
    esp_mqtt_client_destroy(g_client);
    g_client = nullptr;
    g_connected = false;
  }
  if (host.empty()) {
    g_error = "not configured";
    return;
  }
  static std::string s_host, s_user, s_pass, s_id;
  s_host = host;
  s_user = user;
  s_pass = pass;
  s_id = "flapboard-" + g_uid;
  g_will = g_base + "/status";
  esp_mqtt_client_config_t c = {};
  c.broker.address.hostname = s_host.c_str();
  c.broker.address.port = (uint32_t)port;
  c.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
  c.credentials.client_id = s_id.c_str();
  if (!s_user.empty()) c.credentials.username = s_user.c_str();
  if (!s_pass.empty()) c.credentials.authentication.password = s_pass.c_str();
  c.session.last_will.topic = g_will.c_str();
  c.session.last_will.msg = "offline";
  c.session.last_will.qos = 1;
  c.session.last_will.retain = 1;
  c.session.keepalive = 30;
  c.network.reconnect_timeout_ms = 10000;
  c.task.stack_size = 6144;
  g_client = esp_mqtt_client_init(&c);
  if (!g_client) {
    g_error = "could not start the MQTT client";
    return;
  }
  esp_mqtt_client_register_event(g_client, MQTT_EVENT_ANY, onEvent, nullptr);
  esp_mqtt_client_start(g_client);
  g_error = "connecting to " + host;
  note("mqtt: connecting to %s:%d as %s (topics %s/...)", host.c_str(), port, user.empty() ? "anonymous" : user.c_str(),
       g_base.c_str());
}

template <typename T>
void applyConfig(const char *key, T value) {
  JsonDocument p;
  p[key] = value;
  std::string err;
  config::apply(p.as<JsonVariantConst>(), &err);
}

void handle(const Command &c) {
  const std::string t = c.topic, v = c.payload;
  if (t == "display/set") {
    power::requestLatch(v == "OFF");   // OFF holds it off; ON releases (sleep hours still apply)
  } else if (t == "message/set") {
    // Bare text, or {"text": "...", "seconds": 30}. Empty clears it. Not kept
    // across a restart (an old announcement reappearing hours later is worse).
    std::string text = v;
    int seconds = 0;
    JsonDocument d;
    if (!v.empty() && v[0] == '{' && !deserializeJson(d, v)) {
      text = d["text"] | "";
      seconds = d["seconds"] | 0;
    }
    if (text.empty()) content::clearOverride();
    else content::showOverride(text, seconds);
  } else if (t == "show/set") {
    for (int i = 0; i < 4; i++)
      if (v == kShowNames[i]) applyConfig("content_source", kShowSources[i]);
  } else if (t == "volume/set") {
    sound::setVolume(atoi(v.c_str()), true);
  } else if (t == "brightness/set") {
    applyConfig("brightness", std::max(5, std::min(100, atoi(v.c_str()))));
  } else if (t == "sound/set") {
    sound::setEnabled(v == "ON");
  } else if (t == "next/press") {
    content::next();
  }
}

void publishState(bool force) {
  std::string source;
  int brightness;
  {
    config::Reader r;
    source = r.doc()["content_source"] | "messages";
    brightness = r.doc()["brightness"] | 80;
  }
  const char *show = "Messages";
  for (int i = 0; i < 4; i++)
    if (source == kShowSources[i]) show = kShowNames[i];
  JsonDocument p;
  deserializeJson(p, power::statusJson());
  const std::string display = power::isOn() ? "ON" : "OFF";
  const std::string msg = content::overrideText();
  JsonDocument ct;
  deserializeJson(ct, content::statusJson());
  const std::string key = display + "|" + msg + "|" + show + "|" + std::to_string(sound::volume()) + "|" +
                          std::to_string(sound::enabled()) + "|" + std::to_string(brightness) + "|" +
                          std::to_string(motion::active()) + "|" + (ct["reason"] | "") + "|" + (p["reason"] | "");
  const bool beat = millis() - g_last_heartbeat > 30000;
  if (!force && !beat && key == g_last_state) return;
  g_last_state = key;
  g_last_heartbeat = millis();
  publish(g_base + "/status", "online");
  publish(g_base + "/display/state", display);
  publish(g_base + "/message/state", msg.substr(0, 255));
  publish(g_base + "/show/state", show);
  publish(g_base + "/volume/state", std::to_string(sound::volume()));
  publish(g_base + "/brightness/state", std::to_string(brightness));
  publish(g_base + "/sound/state", sound::enabled() ? "ON" : "OFF");
  publish(g_base + "/motion/state", motion::active() ? "ON" : "OFF");
  JsonDocument d;
  d["ip"] = net::ip();
  d["rssi"] = net::rssi();
  d["uptime"] = millis() / 1000;
  d["showing"] = ct["reason"] | "";
  d["power"] = (power::isOn() ? std::string("on: ") : std::string("off: ")) + (p["reason"] | "");
  d["heap_kb"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
  d["wifi_fw"] = net::coprocVersion();
  std::string out;
  serializeJson(d, out);
  publish(g_base + "/diag", out);
}

}  // namespace

void begin() {
  g_cmds = xQueueCreate(8, sizeof(Command));
  g_has_password = !password().empty();   // main loop, at boot
  g_uid = uid();
  g_base = "flapboard/" + g_uid;
}

void loop() {
  static uint32_t last_cfg = 0;
  if (net::state() == net::State::Connected && millis() - last_cfg > 2000) {
    last_cfg = millis();
    connect();
  }
  Command c;
  while (xQueueReceive(g_cmds, &c, 0) == pdTRUE) {
    handle(c);
    publishState(true);
  }
  if (!g_connected) return;
  if (g_need_discovery) {
    g_need_discovery = false;
    note("mqtt: connected; Home Assistant discovery sent");
    sendDiscovery();
    publishState(true);
    return;
  }
  publishState(false);
}

void setServer(const std::string &host, int port, const std::string &user, const std::string &pass) {
  JsonDocument p;
  p["mqtt_host"] = host;
  p["mqtt_port"] = port;
  p["mqtt_user"] = user;
  std::string err;
  config::apply(p.as<JsonVariantConst>(), &err);
  if (!pass.empty()) {
    Preferences pr;
    pr.begin("flapboard", false);
    pr.putString("mqtt_pass", pass.c_str());
    pr.end();
    g_has_password = true;
  }
}

std::string statusJson() {
  JsonDocument d;
  d["connected"] = (bool)g_connected;
  d["state"] = g_connected ? "connected" : g_error;
  d["topic_base"] = g_base;
  d["password_stored"] = (bool)g_has_password;
  std::string out;
  serializeJson(d, out);
  return out;
}

}  // namespace mqtt
}  // namespace flapboard
