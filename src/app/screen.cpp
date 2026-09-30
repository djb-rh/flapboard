#include "screen.h"

#include <M5Unified.h>

#include <string>

#include "config.h"
#include "net.h"
#include "sdcard.h"

namespace flapboard {
namespace screen {
namespace {

std::string g_shown;
uint32_t g_last = 0;

void draw(const std::string &wifi, const std::string &sd) {
  auto &d = M5.Display;
  d.startWrite();
  d.fillScreen(TFT_BLACK);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setTextDatum(middle_center);
  d.setFont(&fonts::FreeSansBold24pt7b);
  d.setTextSize(2);
  d.drawString(config::deviceName().c_str(), d.width() / 2, d.height() / 2 - 80);
  d.setFont(&fonts::FreeSans18pt7b);
  d.setTextSize(1);
  d.setTextColor(0xAD55, TFT_BLACK);
  d.drawString(wifi.c_str(), d.width() / 2, d.height() / 2 + 20);
  d.drawString((config::hostname() + ".local").c_str(), d.width() / 2, d.height() / 2 + 70);
  d.drawString(sd.c_str(), d.width() / 2, d.height() / 2 + 120);
  d.endWrite();
}

}  // namespace

void begin() {
  M5.Display.setRotation(3);   // landscape, the same way up as Tabulous5's default
  M5.Display.fillScreen(TFT_BLACK);
}

void loop() {
  if (millis() - g_last < 500) return;
  g_last = millis();
  const std::string wifi = "Wi-Fi: " + net::statusText();
  const std::string sd = sdcard::mounted() ? std::string("SD card ready (") + sdcard::busWidth() + ")"
                                           : "No SD card";
  const std::string key = wifi + "|" + sd + "|" + config::deviceName();
  if (key == g_shown) return;
  g_shown = key;
  draw(wifi, sd);
}

}  // namespace screen
}  // namespace flapboard
