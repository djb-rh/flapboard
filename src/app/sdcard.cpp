#include "sdcard.h"

#include <M5Unified.h>
#include <SD_MMC.h>
#include <driver/sdmmc_host.h>
#include <esp_heap_caps.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>

#include <cstring>

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
const char *g_problem = "";

// What a volume's first sector says it is.
const char *volumeKind(const uint8_t *s) {
  if (!memcmp(s + 3, "EXFAT   ", 8)) return "exfat";
  if (!memcmp(s + 3, "NTFS    ", 8)) return "ntfs";
  if (!memcmp(s + 82, "FAT32   ", 8) || !memcmp(s + 54, "FAT", 3)) return "fat";
  return nullptr;
}

// The card didn't mount: talk to it directly (same slot and pins as SD_MMC on
// the Tab5: slot 0, IO-MUX, 1-bit is enough) and read its first sectors to
// tell "no card" from "a card in a format FatFs here can't read".
const char *probe() {
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.slot = SDMMC_HOST_SLOT_0;
  host.flags = SDMMC_HOST_FLAG_1BIT;
  host.max_freq_khz = SDMMC_FREQ_DEFAULT;
  sdmmc_slot_config_t slot = {};
  slot.cd = SDMMC_SLOT_NO_CD;
  slot.wp = SDMMC_SLOT_NO_WP;
  slot.width = 1;
  if (sdmmc_host_init() != ESP_OK) return "unknown";
  const char *kind = "none";
  uint8_t *buf = (uint8_t *)heap_caps_malloc(512, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  sdmmc_card_t card;
  if (buf && sdmmc_host_init_slot(host.slot, &slot) == ESP_OK && sdmmc_card_init(&host, &card) == ESP_OK) {
    kind = "unknown";
    if (sdmmc_read_sectors(&card, buf, 0, 1) == ESP_OK) {
      const char *k = volumeKind(buf);   // a "superfloppy": no partition table
      if (!k && buf[510] == 0x55 && buf[511] == 0xAA) {
        const uint8_t type = buf[0x1BE + 4];
        uint32_t lba;
        memcpy(&lba, buf + 0x1BE + 8, 4);
        if (type == 0xEE && sdmmc_read_sectors(&card, buf, 2, 1) == ESP_OK) memcpy(&lba, buf + 32, 4);   // GPT: first entry
        if (type == 0) k = "unformatted";
        else if (lba && sdmmc_read_sectors(&card, buf, lba, 1) == ESP_OK) k = volumeKind(buf);
      }
      if (!k) {
        bool blank = true;
        for (int i = 0; i < 512 && blank; i++) blank = buf[i] == 0 || buf[i] == 0xFF;
        k = blank ? "unformatted" : "unknown";
      }
      // "fat" that still didn't mount is damaged or an odd FAT; formatting fixes both.
      kind = strcmp(k, "fat") == 0 ? "unknown" : k;
    }
  }
  free(buf);
  sdmmc_host_deinit();
  return kind;
}

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
    g_problem = "";
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
  g_problem = probe();
  return false;
}

const char *problem() { return g_mounted ? "" : g_problem; }

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
