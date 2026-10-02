#include "net.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <ESP_HostedOTA.h>
#include <WiFi.h>
#include <esp32-hal-hosted.h>
#include <esp_attr.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lwip/ip_addr.h>
#include <ping/ping_sock.h>

#include <algorithm>
#include <cstring>

#include "config.h"
#include "note.h"

#if __has_include("secrets.h") && !defined(FLAPBOARD_RELEASE)
#include "secrets.h"   // gitignored developer seed: WIFI_SSID / WIFI_PASSWORD
#endif

namespace flapboard {
namespace net {
namespace {

State g_state = State::Off;
bool g_ever_connected = false;
std::string g_ssid;
uint32_t g_join_ms = 0;
SemaphoreHandle_t g_mux;
volatile uint32_t g_busy_ms = 0;

bool g_portal = false;
std::string g_ap;
DNSServer g_dns;
uint32_t g_portal_close_ms = 0;
std::vector<Net> g_nets;
volatile bool g_want_scan = false;
std::string g_pending_ssid, g_pending_pass;
bool g_pending_join = false;
volatile bool g_want_coproc_update = false;
bool g_mdns = false;
const IPAddress kApIp(192, 168, 4, 1);

// ---- link watchdog ----------------------------------------------------------
//
// The link can die while the station still reports itself connected (seen
// after scans, during uploads and while idle: espressif/esp-hosted-mcu#184),
// and nothing on the P4 notices. So the router is pinged every 10 s; 30 s of
// silence means restart -- a rejoin never brought a dead link back on the T48
// build, a restart (which also resets the C6) always did. Never mid-upload.
struct WatchCounters {
  uint32_t magic;
  Watch w;
};
__NOINIT_ATTR WatchCounters g_wc;
constexpr uint32_t kWcMagic = 0x57A7C4E2;

esp_ping_handle_t g_ping = nullptr;
volatile uint32_t g_ping_replies = 0;
volatile bool g_ping_done = false;
bool g_router_answers = false;   // armed only once the router has answered
uint32_t g_ping_ms = 0;
int g_silent_checks = 0;

void onPingReply(esp_ping_handle_t, void *) { g_ping_replies++; }
void onPingEnd(esp_ping_handle_t, void *) { g_ping_done = true; }

void pingRouter() {
  const IPAddress gw = WiFi.gatewayIP();
  if (gw == IPAddress(0, 0, 0, 0)) return;
  esp_ping_config_t c = ESP_PING_DEFAULT_CONFIG();
  IP_ADDR4(&c.target_addr, gw[0], gw[1], gw[2], gw[3]);
  c.count = 3;
  c.interval_ms = 300;
  c.timeout_ms = 1000;
  c.task_stack_size = 3072;
  esp_ping_callbacks_t cb = {};
  cb.on_ping_success = onPingReply;
  cb.on_ping_end = onPingEnd;
  g_ping_replies = 0;
  g_ping_done = false;
  if (esp_ping_new_session(&c, &cb, &g_ping) != ESP_OK) {
    g_ping = nullptr;
    return;
  }
  esp_ping_start(g_ping);
}

// Roaming: the ESP32 never leaves an access point by itself. While the signal
// is weak, look (rarely: scans have wedged this Wi-Fi link before) for a
// clearly stronger access point on the same network, and move to it when
// nothing is being uploaded.
uint32_t g_roam_check = 0;
int g_roams = 0;
std::vector<Net> scanAll(bool allow_dups);
void connect(const std::string &ssid, const std::string &pass, const uint8_t *bssid, int channel);

void roam() {
  // The signal is read every 15 s; two weak readings in a row (< -75 dBm)
  // start a scan for a clearly stronger access point on the same network.
  // A scan stalls things for a couple of seconds, so after one that found
  // nothing better the next waits 2 minutes. (It was every 10 minutes: a
  // sign moved behind something sat at -90 dBm, its link silent, until then.)
  static int weak = 0;
  static uint32_t scan_ok_at = 0;
  if (g_state != State::Connected || g_portal) {
    weak = 0;
    return;
  }
  if (!g_roam_check) g_roam_check = millis() + 30000;   // first look 30 s after joining
  if ((int32_t)(millis() - g_roam_check) < 0) return;
  g_roam_check = millis() + 15000;
  const int now = (int)WiFi.RSSI();
  if (now >= -75) {
    weak = 0;
    return;
  }
  if (++weak < 2 || (int32_t)(millis() - scan_ok_at) < 0 || millis() - g_busy_ms < 30000) return;
  scan_ok_at = millis() + 120000;
  const uint8_t *cur = WiFi.BSSID();
  uint8_t mine[6] = {0};
  if (cur) memcpy(mine, cur, 6);
  for (const Net &n : scanAll(true)) {
    if (n.ssid != g_ssid) continue;
    if (!memcmp(n.bssid, mine, 6)) break;   // ours is the strongest heard: stay
    if (n.rssi < now + 8) break;            // not clearly better
    note("wifi: moving to a stronger access point %02X:%02X:%02X:%02X:%02X:%02X (%d dBm, was %d dBm)", n.bssid[0],
         n.bssid[1], n.bssid[2], n.bssid[3], n.bssid[4], n.bssid[5], n.rssi, now);
    g_roams++;
    WiFi.disconnect(false);   // not the radio: it cannot be restarted
    connect(g_ssid, config::secrets::wifiPass(), n.bssid, n.channel);
    return;
  }
  note("wifi: weak signal (%d dBm) and no clearly stronger access point heard", now);
}

void watchLink() {
  roam();
  if (g_state != State::Connected) {
    g_silent_checks = 0;
    return;
  }
  if (g_ping) {
    if (!g_ping_done) return;
    const bool answered = g_ping_replies > 0;
    esp_ping_delete_session(g_ping);
    g_ping = nullptr;
    if (answered) {
      g_wc.w.pings_ok++;
      g_router_answers = true;
      g_silent_checks = 0;
      return;
    }
    g_wc.w.pings_lost++;
    if (!g_router_answers) return;   // a router that never answers pings: no watchdog
    if (++g_silent_checks < 3) return;
    g_silent_checks = 0;
    if (millis() - g_busy_ms > 20000) {
      g_wc.w.restarts++;
      note("wifi: the link to the router went silent; restarting to recover");
      delay(500);
      ESP.restart();
    }
    return;
  }
  if (millis() - g_ping_ms >= 10000) {
    g_ping_ms = millis();
    pingRouter();
  }
}

// Every access point heard (several may share one network name), strongest
// first. allow_dups: keep each access point, not just each name.
std::vector<Net> scanAll(bool allow_dups) {
  std::vector<Net> out;
  const int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    const std::string s = WiFi.SSID(i).c_str();
    if (s.empty()) continue;
    bool dup = false;
    if (!allow_dups)
      for (auto &o : out) dup |= o.ssid == s;
    if (dup) continue;
    Net x{s, WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN, {0}, WiFi.channel(i)};
    if (const uint8_t *b = WiFi.BSSID(i)) memcpy(x.bssid, b, 6);
    out.push_back(x);
  }
  WiFi.scanDelete();
  std::sort(out.begin(), out.end(), [](const Net &a, const Net &b) { return a.rssi > b.rssi; });
  return out;
}

std::vector<Net> doScan() {
  std::vector<Net> out = scanAll(false);
  std::sort(out.begin(), out.end(), [](const Net &a, const Net &b) { return a.rssi > b.rssi; });
  return out;
}

void startPortal() {
  if (g_portal) return;
  // AP+STA: the station side scans, and joins once a network is chosen.
  if (config::secrets::wifiSsid().empty()) WiFi.disconnect(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(kApIp, kApIp, IPAddress(255, 255, 255, 0));
  WiFi.softAP(g_ap.c_str());
  g_dns.setErrorReplyCode(DNSReplyCode::NoError);
  g_dns.start(53, "*", kApIp);   // every name -> us: phones then open /setup
  g_portal = true;
  g_portal_close_ms = 0;
  auto n = doScan();
  xSemaphoreTake(g_mux, portMAX_DELAY);
  g_nets = n;
  xSemaphoreGive(g_mux);
  note("wifi setup: join the open network %s from a phone", g_ap.c_str());
}

void stopPortal() {
  if (!g_portal) return;
  g_dns.stop();
  WiFi.softAPdisconnect(false);   // the hotspot only; the radio cannot be restarted
  WiFi.mode(WIFI_STA);
  g_portal = false;
  note("wifi setup hotspot closed");
}

void connect(const std::string &ssid, const std::string &pass, const uint8_t *bssid, int channel) {
  g_ssid = ssid;
  g_state = State::Connecting;
  g_join_ms = millis();
  // Scan every channel and take the strongest access point with this name.
  // The default (fast scan) joins the FIRST one heard: with several access
  // points on one network the sign sat on a far one at -93 dBm with a near
  // one available.
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.begin(ssid.c_str(), pass.c_str(), channel, bssid);
  // No modem power saving: the C6 napping between beacons is a suspect in
  // the link dying while idle, and a wall sign has the milliamps to spare.
  WiFi.setSleep(false);
}

void reconnect() {
  const std::string s = config::secrets::wifiSsid();
  if (s.empty()) return;
  g_wc.w.rejoins++;
  WiFi.disconnect(false);   // not the radio: it cannot be restarted
  connect(s, config::secrets::wifiPass(), nullptr, 0);
}

void startMdns() {
  if (g_mdns) return;
  const std::string host = config::hostname();
  if (MDNS.begin(host.c_str())) {
    MDNS.addService("http", "tcp", 80);
    g_mdns = true;
  }
}

}  // namespace

void begin() {
  g_mux = xSemaphoreCreateMutex();
  if (g_wc.magic != kWcMagic || esp_reset_reason() == ESP_RST_POWERON) {
    memset(&g_wc, 0, sizeof(g_wc));
    g_wc.magic = kWcMagic;
  }
  // "FlapBoard-" and two bytes of the P4's factory MAC, so two signs differ.
  // (The Wi-Fi MAC comes from the C6 and reads as zeros this early.)
  const uint64_t mac = ESP.getEfuseMac();
  char ap[32];
  snprintf(ap, sizeof(ap), "FlapBoard-%02X%02X", (unsigned)((mac >> 32) & 0xFF), (unsigned)((mac >> 40) & 0xFF));
  g_ap = ap;
  WiFi.setHostname(config::hostname().c_str());
  // A link-local IPv6 address lets mDNS answer the AAAA query too. Without
  // it, macOS waits 5 s for that answer on every name.local lookup (measured:
  // 5.01 s against 0.007 s with curl -4).
  WiFi.mode(WIFI_STA);
  WiFi.enableIPv6(true);   // after mode(): the station interface must exist
#ifdef WIFI_SSID
  // Developer builds carry a network in the gitignored secrets.h; a release
  // build has none and starts in setup mode.
  if (config::secrets::wifiSsid().empty()) config::secrets::setWifi(WIFI_SSID, WIFI_PASSWORD);
#endif
  const std::string s = config::secrets::wifiSsid();
  if (s.empty()) {
    startPortal();
    return;
  }
  connect(s, config::secrets::wifiPass(), nullptr, 0);
}

void loop() {
  if (g_portal) g_dns.processNextRequest();

  xSemaphoreTake(g_mux, portMAX_DELAY);
  const bool want_join = g_pending_join;
  const std::string ps = g_pending_ssid, pp = g_pending_pass;
  g_pending_join = false;
  xSemaphoreGive(g_mux);
  if (g_want_scan) {
    auto n = doScan();
    xSemaphoreTake(g_mux, portMAX_DELAY);
    g_nets = n;
    xSemaphoreGive(g_mux);
    g_want_scan = false;
  }
  if (want_join) join(ps, pp);

  if (g_want_coproc_update) {
    g_want_coproc_update = false;
    note("wifi chip: downloading and flashing esp-hosted %s...", hostVersion().c_str());
    esp_task_wdt_delete(nullptr);   // minutes of blocking; it restarts afterwards
    const bool ok = updateEspHostedSlave();
    note("wifi chip: update %s; restarting", ok ? "done" : "FAILED");
    delay(1000);
    ESP.restart();
  }

  if (g_state == State::Connecting) {
    if (WiFi.status() == WL_CONNECTED) {
      g_state = State::Connected;
      g_ever_connected = true;
      startMdns();
      note("wifi: joined %s via %s as http://%s/ (%s.local), rssi %d", g_ssid.c_str(), WiFi.BSSIDstr().c_str(),
           WiFi.localIP().toString().c_str(), config::hostname().c_str(), (int)WiFi.RSSI());
      g_roam_check = 0;
      static bool reported = false;
      if (!reported) {
        reported = true;
        note("wifi chip firmware %s, this build expects %s%s", coprocVersion().c_str(), hostVersion().c_str(),
             coprocUpdateAvailable() ? " (UPDATE AVAILABLE: System page)" : "");
      }
      if (g_portal) g_portal_close_ms = millis() + 60000;   // let the phone show the result
    } else if (millis() - g_join_ms > 45000) {   // since the C6's 2.12 firmware a join can take ~25 s
      if (g_ever_connected || (!g_portal && !config::secrets::wifiSsid().empty() && millis() < 5 * 60000)) {
        // It worked before, or this is the first few minutes after boot
        // (router slow, or a one-off failed join as seen in Phase 0): keep
        // trying rather than dropping into setup mode.
        g_join_ms = millis();
        reconnect();
      } else {
        g_state = State::Failed;
        note("wifi: could not join %s", g_ssid.c_str());
        startPortal();
      }
    }
  }
  if (g_state == State::Connected && WiFi.status() != WL_CONNECTED) {
    g_state = State::Connecting;   // the stack reconnects by itself
    g_join_ms = millis();
    note("wifi: link dropped; rejoining");
  }
  if (g_portal && g_portal_close_ms && (int32_t)(millis() - g_portal_close_ms) >= 0) stopPortal();
  // Setup mode with a saved network: retry it every 30 s, but not while a
  // phone is on the hotspot (joining moves the radio's channel under it).
  if (g_portal && g_state == State::Failed && !config::secrets::wifiSsid().empty() &&
      WiFi.softAPgetStationNum() == 0 && millis() - g_join_ms > 30000) {
    connect(config::secrets::wifiSsid(), config::secrets::wifiPass(), nullptr, 0);
  }
  watchLink();
}

State state() { return g_state; }
std::string ssid() { return g_ssid; }
std::string ip() { return g_state == State::Connected ? WiFi.localIP().toString().c_str() : ""; }
int rssi() { return g_state == State::Connected ? (int)WiFi.RSSI() : 0; }

std::string statusText() {
  if (g_portal && g_state != State::Connected) return "setup: " + g_ap;
  switch (g_state) {
    case State::Off: return "off";
    case State::Connecting: return "joining " + g_ssid + "...";
    case State::Connected: return ip();
    case State::Failed: return "could not join " + g_ssid;
  }
  return "";
}

bool portalActive() { return g_portal; }
std::string bssid() { return g_state == State::Connected ? WiFi.BSSIDstr().c_str() : ""; }
std::string portalSsid() { return g_ap; }

std::vector<Net> lastScan() {
  xSemaphoreTake(g_mux, portMAX_DELAY);
  auto n = g_nets;
  xSemaphoreGive(g_mux);
  return n;
}

bool requestScan(uint32_t wait_ms) {
  g_want_scan = true;
  for (uint32_t t = 0; t < wait_ms && g_want_scan; t += 100) vTaskDelay(pdMS_TO_TICKS(100));
  return !g_want_scan;
}

void requestJoin(const std::string &ssid, const std::string &pass) {
  xSemaphoreTake(g_mux, portMAX_DELAY);
  g_pending_ssid = ssid;
  g_pending_pass = pass;
  g_pending_join = true;
  xSemaphoreGive(g_mux);
}

void join(const std::string &ssid, const std::string &pass) {
  config::secrets::setWifi(ssid, pass);
  WiFi.disconnect(false);
  connect(ssid, pass, nullptr, 0);
}

std::string coprocVersion() {
  // Read once (it is an RPC to the C6) and cached; it only changes by an
  // update, which restarts.
  static std::string cached;
  if (!cached.empty()) return cached;
  if (g_state != State::Connected) return "unknown";
  uint32_t a = 0, b = 0, c = 0;
  hostedHasUpdate();   // (re)reads the co-processor's version
  hostedGetSlaveVersion(&a, &b, &c);
  char t[24];
  snprintf(t, sizeof(t), "%lu.%lu.%lu", (unsigned long)a, (unsigned long)b, (unsigned long)c);
  cached = t;
  return cached;
}

std::string hostVersion() {
  uint32_t a = 0, b = 0, c = 0;
  hostedGetHostVersion(&a, &b, &c);
  char t[24];
  snprintf(t, sizeof(t), "%lu.%lu.%lu", (unsigned long)a, (unsigned long)b, (unsigned long)c);
  return t;
}

bool coprocUpdateAvailable() {
  const std::string c = coprocVersion();
  return c != "unknown" && c != hostVersion();
}
void requestCoprocUpdate() { g_want_coproc_update = true; }
Watch watch() { return g_wc.w; }
void markBusy() { g_busy_ms = millis(); }
volatile uint32_t g_upload_ms = 0;
void markUploading() {
  g_upload_ms = millis();
  if (!g_upload_ms) g_upload_ms = 1;
}
bool uploading() { return g_upload_ms && millis() - g_upload_ms < 3000; }

}  // namespace net
}  // namespace flapboard
