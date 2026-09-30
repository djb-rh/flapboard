#include "sdcard.h"

#include <M5Unified.h>
#include <SD_MMC.h>
#include <esp_vfs_fat.h>

// SD_MMC keeps the card handle protected; the whole-card format needs it.
struct CardAccess : fs::SDMMCFS {
  static sdmmc_card_t *card(fs::SDMMCFS &f) { return static_cast<CardAccess &>(f)._card; }
};

namespace flapboard {
namespace sdcard {
namespace {

constexpr const char *kMount = "/sdcard";
bool g_mounted = false;
const char *g_width = "";

void pins(bool wide) {
  // The board definition names the slot's pins in SPI terms; on the Tab5
  // they are the SDIO lines.
  const int clk = M5.getPin(m5::pin_name_t::sd_spi_sclk);
  const int cmd = M5.getPin(m5::pin_name_t::sd_spi_mosi);
  const int d0 = M5.getPin(m5::pin_name_t::sd_spi_miso);
  const int d3 = M5.getPin(m5::pin_name_t::sd_spi_cs);
  if (wide) SD_MMC.setPins(clk, cmd, d0, d0 + 1, d0 + 2, d3);
  else SD_MMC.setPins(clk, cmd, d0);
}

}  // namespace

bool begin() {
  if (g_mounted) return true;
  pins(true);
  if (SD_MMC.begin(kMount, false, false, 20000)) {
    g_mounted = true;
    g_width = "4-bit";
    return true;
  }
  SD_MMC.end();
  pins(false);
  if (SD_MMC.begin(kMount, true, false, 20000)) {
    g_mounted = true;
    g_width = "1-bit";
    return true;
  }
  SD_MMC.end();
  return false;
}

bool mounted() { return g_mounted; }
const char *mountPoint() { return kMount; }
const char *busWidth() { return g_width; }
uint64_t cardBytes() { return g_mounted ? SD_MMC.cardSize() : 0; }

void space(uint64_t *free_b, uint64_t *total_b) {
  *free_b = *total_b = 0;
  if (!g_mounted) return;
  *total_b = SD_MMC.totalBytes();
  *free_b = *total_b - SD_MMC.usedBytes();
}

bool formatWholeCard() {
  // A card that doesn't mount (not FAT) still needs a handle: mount with
  // format_if_mount_failed, which formats it once; then format properly.
  if (!g_mounted) {
    pins(true);
    if (!SD_MMC.begin(kMount, false, true, 20000)) return false;
    g_mounted = true;
    g_width = "4-bit";
  }
  esp_vfs_fat_mount_config_t cfg = {};
  cfg.max_files = 5;
  cfg.allocation_unit_size = 32 * 1024;   // 4 KB would make a ~120 MB FAT on a 128 GB card
  const esp_err_t e = esp_vfs_fat_sdcard_format_cfg(kMount, CardAccess::card(SD_MMC), &cfg);
  SD_MMC.end();
  g_mounted = false;
  return e == ESP_OK && begin();
}

}  // namespace sdcard
}  // namespace flapboard
