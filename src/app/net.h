// Wi-Fi: joining, the setup hotspot (captive portal), mDNS, and the link
// watchdog. Ported from T48-for-Tab5.
//
// The Tab5's radio is an ESP32-C6 behind esp-hosted, and its transport cannot
// be started twice in one boot, so the radio comes up once and stays up;
// changing networks disconnects and rejoins without powering it down.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace flapboard {
namespace net {

void begin();   // joins the saved network, or opens the setup hotspot
void loop();    // from the main loop, which owns the radio

enum class State { Off, Connecting, Connected, Failed };
State state();
std::string ssid();
std::string ip();          // "" until connected
int rssi();
std::string statusText();  // "joining...", "10.0.1.23", "setup: FlapBoard-1A2B"

// The setup hotspot: an open network FlapBoard-XXXX whose page (/setup)
// picks a network. Opens by itself when there is no saved network or the
// first join fails; closes a minute after joining.
bool portalActive();
std::string portalSsid();

struct Net {
  std::string ssid;
  int rssi;
  bool open;
};
// For the web setup page (runs on the server task): the scan and the join
// are handed to the main loop.
std::vector<Net> lastScan();
bool requestScan(uint32_t wait_ms);   // true if a fresh scan finished in time
void requestJoin(const std::string &ssid, const std::string &pass);
void join(const std::string &ssid, const std::string &pass);   // main loop only

// The ESP32-C6's own esp-hosted firmware against this build's host side.
std::string coprocVersion();
std::string hostVersion();
bool coprocUpdateAvailable();
void requestCoprocUpdate();   // done by the main loop; restarts afterwards

// Link watchdog counters (survive a software restart).
struct Watch {
  uint32_t pings_ok, pings_lost, rejoins, restarts;
};
Watch watch();

// The web server reports activity so the watchdog never restarts mid-upload.
void markBusy();

}  // namespace net
}  // namespace flapboard
