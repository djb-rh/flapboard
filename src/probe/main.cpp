// Phase 0 probe: prints everything we need to know about this Tab5 before
// building on it, and can update the ESP32-C6's esp-hosted firmware.
//
// Serial commands (one letter + Enter):
//   r  print the report again        u  update the C6 firmware over Wi-Fi
//   f  format the whole SD card FAT32   w  retry Wi-Fi
//   t  set the RTC from NTP (UTC)       l  list the SD card root
//
// Wi-Fi uses the network T48-for-Tab5 saved in NVS (namespace "burner"), or
// else include/secrets.h (gitignored), so no credentials live in this source.
#include <Arduino.h>
#include <ESP_HostedOTA.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <esp32-hal-hosted.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_idf_version.h>
#include <esp_vfs_fat.h>

#if __has_include("secrets.h")
#include "secrets.h"   // gitignored; WIFI_SSID / WIFI_PASSWORD
#endif

// SD_MMC keeps the card handle protected; the full-card format needs it.
struct CardAccess : fs::SDMMCFS {
  static sdmmc_card_t *card(fs::SDMMCFS &f) { return static_cast<CardAccess &>(f)._card; }
};

namespace {

bool g_sd = false;
const char *g_sd_width = "";

void sdPins(bool wide) {
  const int clk = M5.getPin(m5::pin_name_t::sd_spi_sclk);
  const int cmd = M5.getPin(m5::pin_name_t::sd_spi_mosi);
  const int d0 = M5.getPin(m5::pin_name_t::sd_spi_miso);
  const int d3 = M5.getPin(m5::pin_name_t::sd_spi_cs);
  if (wide) SD_MMC.setPins(clk, cmd, d0, d0 + 1, d0 + 2, d3);
  else SD_MMC.setPins(clk, cmd, d0);
}

void sdBegin() {
  sdPins(true);
  if (SD_MMC.begin("/sdcard", false, false, 20000)) { g_sd = true; g_sd_width = "4-bit"; return; }
  SD_MMC.end();
  sdPins(false);
  if (SD_MMC.begin("/sdcard", true, false, 20000)) { g_sd = true; g_sd_width = "1-bit"; return; }
  SD_MMC.end();
}

// Read a 1 MB file back to measure the card; writes it first if missing.
void sdSpeed() {
  if (!g_sd) return;
  const char *path = "/probe_speed.bin";
  static uint8_t buf[32768];
  File f = SD_MMC.open(path, FILE_WRITE);
  if (!f) { Serial.println("sd: cannot write"); return; }
  uint32_t t0 = millis();
  for (int i = 0; i < 32; ++i) f.write(buf, sizeof(buf));
  f.close();
  const uint32_t tw = millis() - t0;
  f = SD_MMC.open(path);
  t0 = millis();
  while (f.read(buf, sizeof(buf)) > 0) {}
  f.close();
  const uint32_t tr = millis() - t0;
  SD_MMC.remove(path);
  Serial.printf("sd speed: write 1 MB %lu ms, read 1 MB %lu ms\n", (unsigned long)tw, (unsigned long)tr);
}

const char *sdType() {
  switch (SD_MMC.cardType()) {
    case CARD_MMC: return "MMC";
    case CARD_SD: return "SDSC";
    case CARD_SDHC: return "SDHC/SDXC";
    default: return "none";
  }
}

void wifiBegin() {
  Preferences p;
  p.begin("burner", true);
  String ssid = p.getString("ssid", ""), pass = p.getString("pass", "");
  p.end();
#ifdef WIFI_SSID
  if (ssid.isEmpty()) { ssid = WIFI_SSID; pass = WIFI_PASSWORD; }
#endif
  if (ssid.isEmpty()) { Serial.println("wifi: no saved network"); return; }
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.printf("wifi: joining \"%s\"", ssid.c_str());
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; ++i) { delay(500); Serial.print('.'); }
  Serial.println();
}

void report() {
  esp_chip_info_t ci;
  esp_chip_info(&ci);
  uint32_t flash = 0;
  esp_flash_get_size(nullptr, &flash);
  Serial.println("===== FLAPBOARD PROBE =====");
  Serial.printf("chip: %s rev v%d.%d, %d cores, %lu MHz\n", ESP.getChipModel(), ci.revision / 100,
                ci.revision % 100, ci.cores, (unsigned long)ESP.getCpuFreqMHz());
  Serial.printf("flash: %lu MB, psram: %lu MB (free %lu KB)\n", (unsigned long)(flash >> 20),
                (unsigned long)(ESP.getPsramSize() >> 20), (unsigned long)(ESP.getFreePsram() >> 10));
  Serial.printf("internal free: %lu KB, largest DMA block: %lu KB\n",
                (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10),
                (unsigned long)(heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) >> 10));
  Serial.printf("arduino core: %s, esp-idf: %s\n", ESP_ARDUINO_VERSION_STR, esp_get_idf_version());

  // Which panel/touch pair M5GFX found: GT911 means the ILI9881C panel.
  auto *t = M5.Display.touch();
  int addr = t ? t->config().i2c_addr : -1;
  const char *tname = addr == 0x14 || addr == 0x5D ? "GT911 (ILI9881C panel)"
                      : addr == 0x55 ? "ST7123 (ST7123 panel)" : "unknown";
  Serial.printf("display: %dx%d, touch i2c 0x%02X = %s\n", M5.Display.width(), M5.Display.height(), addr, tname);

  auto dt = M5.Rtc.getDateTime();
  Serial.printf("rtc: %s %04d-%02d-%02d %02d:%02d:%02d\n", M5.Rtc.isEnabled() ? "ok" : "MISSING",
                dt.date.year, dt.date.month, dt.date.date, dt.time.hours, dt.time.minutes, dt.time.seconds);
  Serial.printf("battery: %d mV, %d%%, current %d mA\n", M5.Power.getBatteryVoltage(),
                M5.Power.getBatteryLevel(), (int)M5.Power.getBatteryCurrent());

  if (g_sd) {
    Serial.printf("sd: %s, %s bus, card %llu MB, fs total %llu MB, used %llu MB\n", sdType(), g_sd_width,
                  SD_MMC.cardSize() >> 20, SD_MMC.totalBytes() >> 20, SD_MMC.usedBytes() >> 20);
  } else {
    Serial.println("sd: not mounted (no card, or not FAT - 'f' formats it)");
  }

  uint32_t a = 0, b = 0, c = 0;
  hostedGetHostVersion(&a, &b, &c);
  Serial.printf("esp-hosted host (P4 side): %lu.%lu.%lu\n", (unsigned long)a, (unsigned long)b, (unsigned long)c);
  if (WiFi.status() == WL_CONNECTED) {
    const bool upd = hostedHasUpdate();
    hostedGetSlaveVersion(&a, &b, &c);
    Serial.printf("esp-hosted slave (C6): %lu.%lu.%lu -> %s\n", (unsigned long)a, (unsigned long)b,
                  (unsigned long)c, upd ? "UPDATE AVAILABLE" : "matches");
    Serial.printf("wifi: %s ip %s rssi %d ch %d mac %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
                  WiFi.RSSI(), WiFi.channel(), WiFi.macAddress().c_str());
  } else {
    Serial.println("wifi: not connected (C6 version needs the link up)");
  }
  Serial.println("===== END =====");
}

}  // namespace

void setup() {
  auto cfg = M5.config();
  cfg.output_power = false;   // keeps every outgoing rail off (Tab5 lesson)
  M5.begin(cfg);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  M5.Display.setRotation(3);
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextSize(3);
  M5.Display.drawString("FlapBoard probe", 40, 40);
  sdBegin();
  sdSpeed();
  wifiBegin();
  report();
}

void loop() {
  M5.update();
  if (!Serial.available()) { delay(20); return; }
  const int ch = Serial.read();
  if (ch == 'r') report();
  else if (ch == 'w') { wifiBegin(); report(); }
  else if (ch == 'l') {
    File root = SD_MMC.open("/");
    for (File e = root.openNextFile(); e; e = root.openNextFile())
      Serial.printf("  %s %s %lu\n", e.isDirectory() ? "d" : "-", e.name(), (unsigned long)e.size());
  }
  else if (ch == 'f') {
    // Repartitions the whole card (one partition) and writes FAT32 with
    // 32 KB clusters: the default 4 KB would make a ~120 MB FAT on this card.
    if (!g_sd) sdBegin();
    if (!g_sd) { Serial.println("sd: no card to format"); return; }
    esp_vfs_fat_mount_config_t cfg = {};
    cfg.max_files = 5;
    cfg.allocation_unit_size = 32 * 1024;
    Serial.println("sd: formatting whole card as FAT32...");
    const uint32_t t0 = millis();
    const esp_err_t e = esp_vfs_fat_sdcard_format_cfg("/sdcard", CardAccess::card(SD_MMC), &cfg);
    Serial.printf("sd: format %s (%s) in %lu ms\n", e == ESP_OK ? "ok" : "FAILED", esp_err_to_name(e),
                  (unsigned long)(millis() - t0));
    SD_MMC.end();
    g_sd = false;
    sdBegin();
    sdSpeed();
    report();
  } else if (ch == 't') {
    // NTP -> RTC, stored as UTC (timezones are applied on top later).
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    struct tm tm;
    if (!getLocalTime(&tm, 15000)) { Serial.println("ntp: no reply"); return; }
    M5.Rtc.setDateTime(&tm);
    Serial.printf("ntp: set rtc to %04d-%02d-%02d %02d:%02d:%02d UTC\n", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    report();
  } else if (ch == 'u') {
    Serial.println("c6: updating esp-hosted firmware (takes a minute or two)...");
    const bool ok = updateEspHostedSlave();
    Serial.printf("c6: update %s - power cycle, then 'r'\n", ok ? "OK" : "FAILED");
  }
}
