// FlapBoard: a split-flap sign and photo frame for the M5Stack Tab5.
// Phase 1 skeleton: Wi-Fi + setup hotspot, web UI and file library, config,
// serial console. See PLAN.md.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_ota_ops.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <sys/stat.h>

#include <algorithm>

#include "clock.h"
#include "config.h"
#include "content.h"
#include "console.h"
#include "library.h"
#include "motion.h"
#include "mqtt.h"
#include "net.h"
#include "power.h"
#include "note.h"
#include "sign.h"
#include "sound.h"
#include "sdcard.h"
#include "status.h"
#include "weather.h"
#include "web.h"

// Arduino's startup marks a new firmware good before setup() runs unless
// this says otherwise; FlapBoard decides itself, once Wi-Fi is up (loop()).
extern "C" bool verifyRollbackLater() { return true; }

using namespace flapboard;

namespace {

// The folders the rest of the sign reads from. Made at boot so the file
// manager always has somewhere to put things.
void makeLibraryFolders() {
  const std::string root = std::string(sdcard::mountPoint()) + "/flapboard";
  mkdir(root.c_str(), 0777);
  // "images" was the side pictures' folder until 0.9.0; easily taken for the
  // photos' one, so they moved in with the messages they frame. Move an old one over.
  mkdir((root + "/messages").c_str(), 0777);
  struct stat st;
  if (stat((root + "/images").c_str(), &st) == 0 && stat((root + "/messages/side-pictures").c_str(), &st) != 0 &&
      rename((root + "/images").c_str(), (root + "/messages/side-pictures").c_str()) == 0)
    note("sd: moved images/ to messages/side-pictures/");
  for (const char *d : {"photos", "messages", "messages/side-pictures", "sounds", "fonts"})
    mkdir((root + "/" + d).c_str(), 0777);
  library::setRoot(root);
  library::setSpaceProvider(sdcard::space);
}

}  // namespace

static // The web page's "Format SD card" leaves a flag and restarts; the format runs
// here, before Wi-Fi: the card shares the P4's SDIO host with the Wi-Fi chip,
// and the link watchdog must not restart the sign halfway through.
void formatCardIfAsked() {
  Preferences p;
  p.begin("flapboard", false);
  const bool asked = p.getBool("sd_format", false);
  if (asked) p.remove("sd_format");   // cleared first: a failed format must not loop
  p.end();
  if (!asked) return;
  {
    config::Reader r;   // landscape like the sign (sign.cpp picks the same rotation)
    M5.Display.setRotation(std::string(r.doc()["orientation"] | "landscape") == "landscape_flipped" ? 1 : 3);
  }
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(&fonts::DejaVu40);
  M5.Display.drawString("Formatting the SD card...", M5.Display.width() / 2, M5.Display.height() / 2 - 30);
  M5.Display.setFont(&fonts::DejaVu24);
  M5.Display.drawString("About a minute. Don't switch off.", M5.Display.width() / 2, M5.Display.height() / 2 + 30);
  note("sd: formatting the whole card (FAT32), asked from the web page");
  const bool ok = sdcard::formatWholeCard();
  note("sd: format %s", ok ? "done" : "FAILED");
  M5.Display.fillScreen(TFT_BLACK);
}

void mem(const char *stage) {
  note("mem %-14s internal %3u KB, largest DMA %3u KB", stage, (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
       (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) / 1024));
}

void setup() {
  // Ordinary allocations above 32 bytes go to PSRAM (the prebuilt default is
  // 4 KB; 64 was not enough once the photo index held ~400 paths of ~50
  // characters each: tens of KB of internal RAM, and the Wi-Fi link wedged
  // again). Internal RAM is what the Wi-Fi chip's receive buffers need; once
  // the settings JSON, strings and status documents had eaten it (largest DMA
  // block 35 KB), uploads wedged the link and the watchdog restarted the sign.
  // DMA and driver buffers ask for internal memory explicitly and stay there.
  heap_caps_malloc_extmem_enable(32);
  // Settings first, so the relay pin is driven OFF before anything else runs:
  // a reboot must never flash the car's lights.
  config::begin();
  power::beginEarly();
  auto cfg = M5.config();
  cfg.output_power = false;   // every outgoing rail stays off (Tab5 lesson)
  M5.begin(cfg);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);   // never block on a USB console nobody reads

  {
    const auto prev = previousNotes();
    if (!prev.empty()) note("previous run ended with: %s", prev.back().c_str());
  }
  formatCardIfAsked();
  if (sdcard::begin()) makeLibraryFolders();
  else note("sd: not mounted (%s): the file library is unavailable", sdcard::problem());
  {
    // Side pictures chosen before the rename point into images/: follow it.
    JsonDocument patch;
    {
      config::Reader r;
      for (const char *k : {"side_left", "side_right"}) {
        const std::string v = r.doc()[k] | "";
        if (v.rfind("images/", 0) == 0) patch[k] = "messages/side-pictures/" + v.substr(7);
      }
    }
    std::string err;
    if (patch.size()) config::apply(patch.as<JsonVariantConst>(), &err);
  }

  mem("before sound");
  clock::begin();
  sound::begin();
  mem("after sound");
  sign::begin();
  content::begin();
  mem("after sign");
  // The network stack first (lwIP's socket VFS: one of a handful of VFS slots,
  // and the camera's devices would otherwise take the last ones), then the
  // camera, then Wi-Fi itself (which takes the internal RAM the camera needs).
  esp_netif_init();
  motion::beginEarly();
  mem("after camera");
  net::begin();
  mem("after net");
  web::begin();
  mem("after web");
  weather::begin();
  mqtt::begin();
  mem("after mqtt");
  // ESP-IDF's own logging goes quiet from here: with the Mac attached and
  // nothing reading serial, blocking log writes were suspected of killing
  // Wi-Fi uploads on the T48 build. note() still prints.
  esp_log_level_set("*", ESP_LOG_NONE);

  // The main loop reads touch and owns the radio: if it blocks, panic so the
  // core dump names the call, rather than a silent freeze.
  esp_task_wdt_config_t wdt = {.timeout_ms = 15000, .idle_core_mask = 0, .trigger_panic = true};
  esp_task_wdt_reconfigure(&wdt);
  esp_task_wdt_add(nullptr);

  note("FlapBoard %s up; reset reason %d", FLAPBOARD_VERSION, (int)esp_reset_reason());
  M5.Touch.setHoldThresh(800);
  console::begin();
}

// Long press anywhere (0.8 s) opens the quick panel; while it is open, taps
// go to its buttons. Touch is read here, on the main loop, the only task that
// talks to the touch chip.
void handleTouch() {
  if (M5.Touch.getCount() == 0 && !M5.Touch.getDetail().wasReleased()) return;
  const auto t = M5.Touch.getDetail();
  if (!power::isOn()) {   // dark: a tap only wakes it
    if (t.wasPressed()) power::wake();
    return;
  }
  if (sign::panelOpen()) {
    if (t.wasClicked() || t.wasHold()) sign::panelTap(t.x, t.y);
  } else if (t.wasClicked() || t.wasHold()) {   // one tap (or a hold, as before) opens the sheet
    sign::openPanel();
  }
}

void loop() {
  esp_task_wdt_reset();
  M5.update();
  handleTouch();
  net::loop();
  sound::loop();
  clock::loop();
  content::loop();
  power::loop();
  motion::loop();
  mqtt::loop();
  status::update();
  console::loop();
  sign::Action a;
  while (sign::takeAction(&a)) {   // info-sheet buttons: settings are written here, on the main loop
    std::string err;
    JsonDocument p;
    if (a.kind == sign::ActionKind::Brightness) {
      int b;
      {
        config::Reader r;
        b = r.doc()["brightness"] | 80;
      }
      p["brightness"] = std::max(5, std::min(100, b + a.arg));
      config::apply(p.as<JsonVariantConst>(), &err);
    } else if (a.kind == sign::ActionKind::Source) {
      p["content_source"] = sign::sourceForMode(a.arg);
      config::apply(p.as<JsonVariantConst>(), &err);
    } else if (a.kind == sign::ActionKind::Next) {
      content::next();
    } else if (a.kind == sign::ActionKind::ShowIp) {
      content::showOverride("{name}||{hostname}|{ip}", 30);
    }
  }
  // A firmware that arrived over the air runs on trial (bootloader rollback):
  // once it has been up 30 s with Wi-Fi joined and the web server answering,
  // it is kept; a firmware that never gets that far is rolled back on restart.
#ifdef FLAPBOARD_CRASH_TEST
  if (millis() > 10000) abort();   // test build only: proves a bad update rolls back
#endif
  static bool marked = false;
  if (!marked && millis() > 30000 && net::state() == net::State::Connected) {
    marked = true;
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
      esp_ota_mark_app_valid_cancel_rollback();
      note("update: firmware %s on %s confirmed after a healthy start", FLAPBOARD_VERSION, run->label);
    }
  }
  // An hourly line in the log (kept across restarts): enough to see a slow
  // leak or a fading signal over a long run.
  static uint32_t health_ms = 0;
  if (millis() - health_ms > 3600000UL) {
    health_ms = millis();
    if (millis() > 60000)
      note("health: up %luh, internal %u KB (largest DMA %u KB), PSRAM %u KB, wifi %d dBm", (unsigned long)(millis() / 3600000UL),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), net::rssi());
  }
  if (web::takeRebootRequest()) {
    note("restarting (asked from the web)");
    delay(300);
    ESP.restart();
  }
  delay(5);
}
