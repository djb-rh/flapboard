// FlapBoard: a split-flap sign and photo frame for the M5Stack Tab5.
// Phase 1 skeleton: Wi-Fi + setup hotspot, web UI and file library, config,
// serial console. See PLAN.md.
#include <Arduino.h>
#include <M5Unified.h>
#include <esp_log.h>
#include <esp_task_wdt.h>
#include <sys/stat.h>

#include "config.h"
#include "console.h"
#include "library.h"
#include "net.h"
#include "note.h"
#include "sign.h"
#include "sdcard.h"
#include "status.h"
#include "web.h"

using namespace flapboard;

namespace {

// The folders the rest of the sign reads from. Made at boot so the file
// manager always has somewhere to put things.
void makeLibraryFolders() {
  const std::string root = std::string(sdcard::mountPoint()) + "/flapboard";
  mkdir(root.c_str(), 0777);
  for (const char *d : {"photos", "messages", "images", "sounds", "fonts"})
    mkdir((root + "/" + d).c_str(), 0777);
  library::setRoot(root);
  library::setSpaceProvider(sdcard::space);
}

}  // namespace

void setup() {
  auto cfg = M5.config();
  cfg.output_power = false;   // every outgoing rail stays off (Tab5 lesson)
  M5.begin(cfg);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);   // never block on a USB console nobody reads

  config::begin();
  if (sdcard::begin()) makeLibraryFolders();
  else note("sd: no card (or not FAT32): the file library is unavailable");

  sign::begin();
  net::begin();
  web::begin();
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
  console::begin();
}

void loop() {
  esp_task_wdt_reset();
  M5.update();
  net::loop();
  status::update();
  console::loop();
  if (web::takeRebootRequest()) {
    note("restarting (asked from the web)");
    delay(300);
    ESP.restart();
  }
  delay(5);
}
