#include "clock.h"

#include <Arduino.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <esp_sntp.h>
#include <sys/time.h>

#include <cstring>

#include "config.h"
#include "generated/zones.h"
#include "net.h"
#include "note.h"

namespace flapboard {
namespace clock {
namespace {

volatile bool g_synced = false;      // NTP this boot
volatile bool g_write_rtc = false;   // a sync happened; the main loop writes the RTC (I2C)
volatile uint32_t g_sync_ms = 0;
bool g_from_rtc = false;
bool g_ntp_started = false;
std::string g_zone = "UTC", g_posix = "UTC0", g_servers;

void onSync(struct timeval *) {   // lwIP's task: only flags here
  g_synced = true;
  g_write_rtc = true;
  g_sync_ms = millis();
}

void applyZone() {
  std::string iana, servers;
  {
    config::Reader r;
    iana = r.doc()["timezone"] | "America/New_York";
    servers = r.doc()["ntp_server"] | "pool.ntp.org";
  }
  if (iana == g_zone && servers == g_servers) return;
  std::string posix = posixFor(iana);
  if (posix.empty()) {
    note("clock: unknown time zone \"%s\"; using UTC", iana.c_str());
    iana = "UTC";
    posix = "UTC0";
  }
  g_zone = iana;
  g_posix = posix;
  setenv("TZ", posix.c_str(), 1);
  tzset();
  if (servers != g_servers) {
    g_servers = servers;
    if (g_ntp_started) {   // restart with the new server
      esp_sntp_stop();
      esp_sntp_setservername(0, g_servers.c_str());
      esp_sntp_init();
    }
  }
  note("clock: time zone %s (%s)", g_zone.c_str(), g_posix.c_str());
}

}  // namespace

std::string posixFor(const std::string &iana) {
  const std::string key = iana + "\t";
  const char *p = strstr(zones::kZones, key.c_str());
  while (p && p != zones::kZones && p[-1] != '\n') p = strstr(p + 1, key.c_str());   // whole names only
  if (!p) return "";
  p += key.size();
  const char *e = strchr(p, '\n');
  return e ? std::string(p, e - p) : std::string(p);
}

void begin() {
  applyZone();
  // The RTC keeps UTC. Trust it only if NTP ever set it (a fresh RTC reads
  // 2002 here, and acting on that would fire every schedule wrongly).
  Preferences p;
  p.begin("flapboard", true);
  const bool ever = p.getBool("rtc_ntp", false);
  p.end();
  auto dt = M5.Rtc.getDateTime();
  if (ever && dt.date.year >= 2025 && dt.date.year < 2100) {
    struct tm t = {};
    t.tm_year = dt.date.year - 1900;
    t.tm_mon = dt.date.month - 1;
    t.tm_mday = dt.date.date;
    t.tm_hour = dt.time.hours;
    t.tm_min = dt.time.minutes;
    t.tm_sec = dt.time.seconds;
    // mktime would apply TZ; the RTC is UTC, so convert by hand.
    setenv("TZ", "UTC0", 1);
    tzset();
    const time_t utc = mktime(&t);
    setenv("TZ", g_posix.c_str(), 1);
    tzset();
    struct timeval tv = {utc, 0};
    settimeofday(&tv, nullptr);
    g_from_rtc = true;
    note("clock: set from the RTC (%04d-%02d-%02d %02d:%02d UTC)", dt.date.year, dt.date.month, dt.date.date,
         dt.time.hours, dt.time.minutes);
  } else {
    note("clock: RTC not trusted yet (year %d); waiting for NTP", dt.date.year);
  }
}

void loop() {
  static uint32_t last_cfg = 0;
  if (millis() - last_cfg > 2000) {
    last_cfg = millis();
    applyZone();
  }
  if (!g_ntp_started && net::state() == net::State::Connected) {
    g_ntp_started = true;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, g_servers.c_str());
    sntp_set_time_sync_notification_cb(onSync);
    sntp_set_sync_interval(3 * 3600 * 1000);   // every 3 h; the RTC covers the gaps
    esp_sntp_init();
  }
  if (g_write_rtc) {   // here, on the main loop: the RTC is on the shared I2C bus
    g_write_rtc = false;
    time_t now = time(nullptr);
    struct tm u;
    gmtime_r(&now, &u);
    M5.Rtc.setDateTime(&u);
    Preferences p;   // flash write: main loop only (internal-RAM stack)
    p.begin("flapboard", false);
    p.putBool("rtc_ntp", true);
    p.end();
    char b[32];
    struct tm l;
    localtime_r(&now, &l);
    strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &l);
    note("clock: NTP sync, %s %s; RTC updated", b, g_zone.c_str());
  }
}

bool trusted() { return g_synced || g_from_rtc; }

bool localNow(struct tm *out) {
  memset(out, 0, sizeof(*out));
  if (!trusted()) return false;
  time_t now = time(nullptr);
  localtime_r(&now, out);
  return true;
}

std::string zone() { return g_zone; }

std::string statusText() {
  if (g_synced) {
    const uint32_t m = (millis() - g_sync_ms) / 60000;
    return m == 0 ? "synced just now" : "synced " + std::to_string(m) + " min ago";
  }
  return g_from_rtc ? "from the RTC (NTP not reached yet)" : "not set";
}

}  // namespace clock
}  // namespace flapboard
