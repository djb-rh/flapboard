// Commands:
//   help                  this list
//   status                the same JSON as /api/status
//   log                   recent notes
//   ls <dir>  crc <file>  rm <file>  mkdir <dir>
//   get <file>            hex lines between BEGIN <size> and END <crc32>
//   put <file> <size>     'put: ready', then <size> raw bytes; ACK (0x06) per 4 KB
//   shot                  SHOT <w> <h>, raw RGB565 rows, ENDSHOT (what is on the glass)
//   mem                   free internal / DMA / PSRAM
//   wifi <ssid> [pass]    join and remember a network
//   config [json]         print the settings, or apply a JSON patch
//   c6update              update the Wi-Fi chip's firmware (restarts)
//   sdformat ERASE        repartition + FAT32 the whole card; erases it
//   reboot
#include "console.h"

#include <ArduinoJson.h>
#include <M5Unified.h>
#include <dirent.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_vfs.h>
#include <esp_task_wdt.h>
#include <esp_rom_crc.h>
#include <lgfx/v1/platforms/esp32p4/Panel_DSI.hpp>
#include <sys/stat.h>

#include <string>
#include <vector>

#include "config.h"
#include "content.h"
#include "net.h"
#include "power.h"
#include "note.h"
#include "sdcard.h"
#include "sign.h"
#include "sound.h"
#include "status.h"

namespace flapboard {
namespace console {
namespace {

std::string g_line;

std::vector<std::string> split(const std::string &s) {
  std::vector<std::string> out;
  std::string cur;
  bool quoted = false;
  for (char c : s) {
    if (c == '"') quoted = !quoted;
    else if (c == ' ' && !quoted) {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

void done(int rc) { Serial.printf("== done %d\n", rc); }

int cmdLs(const std::string &dir) {
  DIR *d = opendir(dir.c_str());
  if (!d) return 1;
  while (dirent *e = readdir(d)) {
    struct stat st;
    stat((dir + "/" + e->d_name).c_str(), &st);
    Serial.printf("%10ld %s%s\n", (long)st.st_size, e->d_name, S_ISDIR(st.st_mode) ? "/" : "");
  }
  closedir(d);
  return 0;
}

int cmdCrc(const std::string &path) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return 1;
  static uint8_t *buf = (uint8_t *)heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
  uint32_t crc = 0, size = 0;
  size_t n;
  while ((n = fread(buf, 1, 16384, f)) > 0) {
    esp_task_wdt_reset();
    crc = esp_rom_crc32_le(crc, buf, n);
    size += n;
  }
  fclose(f);
  Serial.printf("%s: %lu bytes crc32 %08lx\n", path.c_str(), (unsigned long)size, (unsigned long)crc);
  return 0;
}

int cmdGet(const std::string &path) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return 1;
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  Serial.setTxTimeoutMs(1000);
  Serial.printf("BEGIN %ld\n", size);
  uint8_t in[64];
  char line[140];
  uint32_t crc = 0;
  size_t n;
  while ((n = fread(in, 1, sizeof(in), f)) > 0) {
    esp_task_wdt_reset();
    crc = esp_rom_crc32_le(crc, in, n);
    int o = 0;
    for (size_t k = 0; k < n; k++) o += sprintf(line + o, "%02x", in[k]);
    line[o++] = '\n';
    Serial.write((const uint8_t *)line, o);
  }
  fclose(f);
  Serial.printf("END %08lx\n", (unsigned long)crc);
  Serial.flush();
  Serial.setTxTimeoutMs(0);
  return 0;
}

// Raw bytes after the command line, acknowledged in 4 KB steps so the link
// never outruns the card or the receive buffer.
int cmdPut(const std::string &path, long size) {
  for (size_t p = path.find('/', 1); p != std::string::npos; p = path.find('/', p + 1))
    mkdir(path.substr(0, p).c_str(), 0777);
  const std::string tmp = path + ".part";
  FILE *f = fopen(tmp.c_str(), "wb");
  if (!f) return 1;
  static uint8_t *buf = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
  long left = size;
  uint32_t crc = 0;
  Serial.setTxTimeoutMs(1000);
  Serial.println("put: ready");
  while (left > 0) {
    esp_task_wdt_reset();
    const int want = left < 4096 ? (int)left : 4096;
    int got = 0;
    const uint32_t t0 = millis();
    while (got < want && millis() - t0 < 5000) {
      const int a = Serial.available();
      if (a > 0) got += Serial.readBytes(buf + got, std::min(a, want - got));
      else delay(1);
    }
    if (got < want || fwrite(buf, 1, got, f) != (size_t)got) {
      fclose(f);
      remove(tmp.c_str());
      Serial.setTxTimeoutMs(0);
      Serial.printf("\nput: failed with %ld bytes left\n", left);
      return 1;
    }
    crc = esp_rom_crc32_le(crc, buf, got);
    left -= got;
    Serial.write(0x06);   // ACK: a byte no log line contains
  }
  fclose(f);
  remove(path.c_str());
  rename(tmp.c_str(), path.c_str());
  Serial.printf("\nput: %s %ld bytes crc32 %08lx\n", path.c_str(), size, (unsigned long)crc);
  Serial.setTxTimeoutMs(0);
  return 0;
}

// What is on the glass, read back from the DSI framebuffer and un-rotated
// (the same mapping as Panel_FrameBufferBase::drawPixelPreclipped).
int cmdShot() {
  auto *panel = (lgfx::Panel_DSI *)M5.Display.getPanel();
  const uint8_t *fb = (const uint8_t *)panel->config_detail().buffer;
  if (!fb) return 1;
  const size_t stride = ((size_t)panel->config().panel_width * 2 + 3) & ~(size_t)3;
  const uint8_t rot = (uint8_t)M5.Display.getRotation();
  const int pw = panel->config().panel_width, ph = panel->config().panel_height;
  const int w = (rot & 1) ? ph : pw, h = (rot & 1) ? pw : ph;
  Serial.setTxTimeoutMs(10000);
  Serial.printf("SHOT %d %d\n", w, h);
  static uint16_t *row = (uint16_t *)heap_caps_malloc(1280 * 2, MALLOC_CAP_SPIRAM);
  for (int y = 0; y < h; y++) {
    esp_task_wdt_reset();
    for (int x = 0; x < w; x++) {
      size_t prow, pcol;
      switch (rot) {
        case 0: prow = y; pcol = x; break;
        case 1: prow = x; pcol = pw - 1 - y; break;
        case 2: prow = ph - 1 - y; pcol = pw - 1 - x; break;
        default: prow = ph - 1 - x; pcol = y; break;
      }
      row[x] = *(const uint16_t *)(fb + prow * stride + pcol * 2);
    }
    Serial.write((const uint8_t *)row, (size_t)w * 2);
  }
  Serial.flush();
  Serial.println("ENDSHOT");
  Serial.setTxTimeoutMs(0);
  return 0;
}

int cmdMem() {
  Serial.printf("internal free %u, dma largest %u, psram free %u\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  return 0;
}

int run(const std::string &line) {
  const auto a = split(line);
  if (a.empty()) return 0;
  const std::string &c = a[0];
  if (c == "help") {
    Serial.println("status log ls crc rm mkdir get put shot mem wifi config c6update sdformat reboot");
    return 0;
  }
  if (c == "status") {
    Serial.println(status::json().c_str());
    return 0;
  }
  if (c == "log") {
    for (auto &l : recentNotes()) Serial.println(l.c_str());
    return 0;
  }
  if (c == "ls") return cmdLs(a.size() > 1 ? a[1] : "/sdcard");
  if (c == "crc" && a.size() > 1) return cmdCrc(a[1]);
  if (c == "rm" && a.size() > 1) return remove(a[1].c_str()) == 0 ? 0 : 1;
  if (c == "mkdir" && a.size() > 1) return mkdir(a[1].c_str(), 0777) == 0 ? 0 : 1;
  // Binary transfers: keep other tasks' notes off the port meanwhile.
  if ((c == "get" && a.size() > 1) || (c == "put" && a.size() > 2) || c == "shot") {
    setSerialQuiet(true);
    const int rc = c == "get" ? cmdGet(a[1]) : c == "put" ? cmdPut(a[1], atol(a[2].c_str())) : cmdShot();
    setSerialQuiet(false);
    return rc;
  }
  if (c == "mem") return cmdMem();
  // Touch stand-ins for testing without a finger: what a long press / tap does.
  if (c == "vfs") {   // which file systems hold the VFS table's 8 slots
    esp_vfs_dump_registered_paths(stdout);
    fflush(stdout);
    return 0;
  }
  if (c == "wake") {   // what a tap on the dark screen does
    power::wake();
    return 0;
  }
  if (c == "hold") {
    sign::openPanel();
    return 0;
  }
  if (c == "tap" && a.size() > 2) {
    if (sign::panelOpen()) sign::panelTap(atoi(a[1].c_str()), atoi(a[2].c_str()));
    return 0;
  }
  if (c == "audiocap") {   // audiocap [seconds] -> /sdcard/flapboard/capture.wav
    sound::capture("/sdcard/flapboard/capture.wav", a.size() > 1 ? (float)atof(a[1].c_str()) : 10.0f);
    return 0;
  }
  if (c == "bench") {
    sign::runBench();
    return 0;
  }
  if (c == "msg" && a.size() > 1) {
    content::showOverride(line.substr(line.find(' ') + 1), 60);
    return 0;
  }
  if (c == "wifi" && a.size() > 1) {
    net::join(a[1], a.size() > 2 ? a[2] : "");
    return 0;
  }
  if (c == "config") {
    if (a.size() == 1) {
      Serial.println(config::toJson().c_str());
      return 0;
    }
    JsonDocument d;
    const std::string js = line.substr(line.find(' ') + 1);
    if (deserializeJson(d, js)) return 2;
    std::string err;
    if (!config::apply(d.as<JsonVariantConst>(), &err)) {
      Serial.println(err.c_str());
      return 1;
    }
    return 0;
  }
  if (c == "c6update") {
    net::requestCoprocUpdate();
    return 0;
  }
  if (c == "sdformat" && a.size() > 1 && a[1] == "ERASE") {
    note("sd: formatting the whole card (FAT32)...");
    esp_task_wdt_delete(nullptr);   // ~30 s on a 128 GB card
    const bool ok = sdcard::formatWholeCard();
    esp_task_wdt_add(nullptr);
    note("sd: format %s", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
  }
  if (c == "reboot") {
    done(0);
    Serial.flush();
    delay(200);
    ESP.restart();
  }
  Serial.printf("unknown command: %s\n", c.c_str());
  return 2;
}

}  // namespace

void begin() { Serial.println("ready"); }

void loop() {
  while (Serial.available()) {
    const int ch = Serial.read();
    if (ch == '\r') continue;
    if (ch == '\n') {
      const std::string line = g_line;
      g_line.clear();
      done(run(line));
      return;   // one command per loop pass
    }
    if (g_line.size() < 512) g_line += (char)ch;
  }
}

}  // namespace console
}  // namespace flapboard
