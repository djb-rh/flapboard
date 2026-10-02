#include "sign.h"

#include <Arduino.h>
#include <SD_MMC.h>   // before M5Unified, so M5GFX can draw images from it
#include <M5Unified.h>
#include <esp_cache.h>
#include <driver/ppa.h>
#include <esp_heap_caps.h>
#include <lgfx/v1/platforms/esp32p4/Panel_DSI.hpp>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <ArduinoJson.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <flapcore/board.h>
#include <flapcore/drum.h>
#include <flapcore/glyphs.h>
#include <flapcore/layout.h>
#include <flapcore/message.h>
#include <flapcore/render.h>
#include <flapcore/theme.h>
#include <flapcore/truetype.h>
#include <flapcore/template.h>

#include "generated/fonts.h"
#include "clock.h"
#include "config.h"
#include "net.h"
#include "note.h"
#include "sdcard.h"
#include "status.h"
#include "sound.h"

namespace flapboard {
namespace sign {
namespace {

using namespace flapcore;

void *psramAlloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
const flapcore::Allocator kPsram{psramAlloc, heap_caps_free};

// Straight into the DSI framebuffer. The panel is portrait (720x1280) and the
// sign landscape. In rotation 3 logical (x, y) is panel row ph-1-x, column y;
// in rotation 1 (the other way up) it is row x, column pw-1-y. Either way each
// logical column of a cell is one evenly spaced run in panel memory (upwards
// in rotation 1), which flapcore composes into directly. M5GFX's pushImage
// rotated pixel by pixel and cost ~0.55 ms per cell.
struct PanelSurface : Surface {
  uint8_t *fb = nullptr;
  size_t stride = 0;   // bytes per panel row
  int pw = 0, ph = 0, rot = 3;
  uint64_t blit_us = 0, sync_us = 0;

  bool begin() {
    auto *panel = (lgfx::Panel_DSI *)M5.Display.getPanel();
    fb = (uint8_t *)panel->config_detail().buffer;
    pw = panel->config().panel_width;
    ph = panel->config().panel_height;
    stride = ((size_t)pw * 2 + 3) & ~(size_t)3;
    rot = M5.Display.getRotation();
    if (rot != 1 && rot != 3) fb = nullptr;
    return fb != nullptr;
  }
  uint16_t *at(int x, int y) const {
    return rot == 3 ? (uint16_t *)(fb + (size_t)(ph - 1 - x) * stride) + y
                    : (uint16_t *)(fb + (size_t)x * stride) + (pw - 1 - y);
  }
  void sync(int x, int w) {
    const int64_t t = esp_timer_get_time();
    const int row0 = rot == 3 ? ph - (x + w) : x;
    esp_cache_msync(fb + (size_t)row0 * stride, (size_t)w * stride,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    sync_us += (uint64_t)(esp_timer_get_time() - t);
  }
  void blit(int x, int y, int w, int h, const uint16_t *px) override {
    const int64_t t = esp_timer_get_time();
    const int step = rot == 3 ? 1 : -1;
    for (int c = 0; c < w; c++) {
      uint16_t *dst = at(x + c, y);
      for (int r = 0; r < h; r++) dst[r * step] = px[(size_t)r * w + c];
    }
    sync(x, w);
    blit_us += (uint64_t)(esp_timer_get_time() - t);
  }
  void fill(int x, int y, int w, int h, uint16_t c) override { M5.Display.fillRect(x, y, w, h, c); }
  uint16_t *column(int x, int y, int *step) override {
    if (!fb) return nullptr;
    *step = rot == 3 ? 1 : -1;
    return at(x, y);
  }
  void columnsDone(int x, int y, int w, int h) override { sync(x, w); }
};

SemaphoreHandle_t g_mux;
volatile bool g_bench = false;

// ---- settings -----------------------------------------------------------------

struct Settings {
  int rows = 6, cols = 22, flap_w = 0, gap = 4, margin = 12, side_pct = 15;
  float aspect = 1.4f, cap = 0.62f;
  std::string theme = "solari", c_bg, c_flap, c_glyph, font = "BebasNeue-Regular";
  std::string left, right, side_fit = "contain", side_bg = "#000000";
  bool flipped = false;
  Motion motion;

  // Everything that needs the glyphs rebuilt and the screen redrawn.
  std::string layoutKey() const {
    char b[160];
    snprintf(b, sizeof(b), "%d|%d|%d|%d|%d|%d|%.3f|%.3f|%d|", rows, cols, flap_w, gap, margin, side_pct, aspect, cap,
             flipped);
    return b + theme + "|" + c_bg + "|" + c_flap + "|" + c_glyph + "|" + font + "|" + left + "|" + right + "|" +
           side_fit + "|" + side_bg;
  }
};

template <typename T>
T clampv(T v, T lo, T hi) { return v < lo ? lo : v > hi ? hi : v; }

Settings readSettings() {
  Settings s;
  config::Reader r;
  auto &d = r.doc();
  s.rows = clampv<int>(d["board_rows"] | 6, 1, 12);
  s.cols = clampv<int>(d["board_cols"] | 22, 1, 40);
  s.flap_w = clampv<int>(d["flap_width"] | 0, 0, 400);
  s.aspect = clampv<float>(d["flap_aspect"] | 1.4f, 0.8f, 2.5f);
  s.gap = clampv<int>(d["flap_gap"] | 4, 0, 40);
  s.margin = clampv<int>(d["board_margin"] | 12, 0, 200);
  s.cap = clampv<float>((d["glyph_size"] | 62) / 100.0f, 0.3f, 0.9f);
  s.theme = d["theme"] | "solari";
  s.c_bg = d["color_background"] | "";
  s.c_flap = d["color_flap"] | "";
  s.c_glyph = d["color_glyph"] | "";
  s.font = d["font"] | "BebasNeue-Regular";
  s.left = d["side_left"] | "";
  s.right = d["side_right"] | "";
  s.side_pct = clampv<int>(d["side_width"] | 15, 5, 40);
  s.side_fit = d["side_fit"] | "contain";
  s.side_bg = d["side_background"] | "#000000";
  s.flipped = std::string(d["orientation"] | "landscape") == "landscape_flipped";
  s.motion.flip_ms = clampv<float>(d["flip_ms"] | 70, 20, 400);
  s.motion.speed_variance = clampv<float>((d["speed_variance"] | 3) / 100.0f, 0, 0.2f);
  const std::string start = d["start_mode"] | "random";
  s.motion.start = start == "together" ? StartMode::Together : start == "wave" ? StartMode::Wave : StartMode::Random;
  return s;
}

// ---- fonts and side images ------------------------------------------------------

// A built-in face, or a .ttf from /flapboard/fonts on the card (kept in PSRAM
// for as long as it is in use).
std::vector<uint8_t> g_font_file;
std::string g_font_used;

bool loadFont(TrueTypeFont &font, const std::string &name) {
  for (const auto &f : fonts::kFonts) {
    if (name == f.name && font.load(f.data, f.len)) {
      g_font_used = f.name;
      return true;
    }
  }
  if (sdcard::mounted() && name.find('/') == std::string::npos && name.find("..") == std::string::npos) {
    const std::string path = std::string(sdcard::mountPoint()) + "/flapboard/fonts/" + name + ".ttf";
    if (FILE *f = fopen(path.c_str(), "rb")) {
      fseek(f, 0, SEEK_END);
      const long n = ftell(f);
      fseek(f, 0, SEEK_SET);
      if (n > 0 && n < 8 * 1024 * 1024) {
        g_font_file.assign((size_t)n, 0);
        if (fread(g_font_file.data(), 1, (size_t)n, f) == (size_t)n && font.load(g_font_file.data(), g_font_file.size())) {
          fclose(f);
          g_font_used = name;
          return true;
        }
      }
      fclose(f);
    }
    note("sign: font %s not found or unreadable; using the built-in one", name.c_str());
  }
  font.load(fonts::kFonts[0].data, fonts::kFonts[0].len);
  for (const auto &f : fonts::kFonts)
    if (strcmp(f.name, "BebasNeue-Regular") == 0) font.load(f.data, f.len);
  g_font_used = "BebasNeue-Regular";
  return false;
}

// Width/height from a PNG or JPEG header, for "cover" scaling.
bool imageSize(const std::string &path, int *w, int *h) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return false;
  uint8_t b[32];
  bool ok = false;
  if (fread(b, 1, 24, f) == 24) {
    if (!memcmp(b, "\x89PNG", 4)) {
      *w = b[16] << 24 | b[17] << 16 | b[18] << 8 | b[19];
      *h = b[20] << 24 | b[21] << 16 | b[22] << 8 | b[23];
      ok = true;
    } else if (b[0] == 0xFF && b[1] == 0xD8) {
      fseek(f, 2, SEEK_SET);
      for (int guard = 0; guard < 200 && fread(b, 1, 4, f) == 4 && b[0] == 0xFF; guard++) {
        const int len = b[2] << 8 | b[3];
        if (b[1] >= 0xC0 && b[1] <= 0xC2) {
          if (fread(b, 1, 5, f) == 5) {
            *h = b[1] << 8 | b[2];
            *w = b[3] << 8 | b[4];
            ok = true;
          }
          break;
        }
        fseek(f, len - 2, SEEK_CUR);
      }
    }
  }
  fclose(f);
  return ok && *w > 0 && *h > 0;
}

void drawSideImage(const Rect &r, const std::string &rel, const Settings &s) {
  if (r.w <= 0 || rel.empty()) return;
  uint16_t bg = 0;
  Theme::parseHex(s.side_bg, &bg);
  M5.Display.fillRect(r.x, r.y, r.w, r.h, bg);
  if (!sdcard::mounted() || rel.find("..") != std::string::npos) return;
  const std::string vfs = std::string(sdcard::mountPoint()) + "/flapboard/" + rel;   // POSIX path
  const std::string fsp = "/flapboard/" + rel;                                       // SD_MMC (Arduino FS) path
  std::string lower = rel;
  for (auto &c : lower) c = (char)tolower((unsigned char)c);
  const bool png = lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".png") == 0;
  const bool jpg = (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".jpg") == 0) ||
                   (lower.size() > 5 && lower.compare(lower.size() - 5, 5, ".jpeg") == 0);
  float zx = 0, zy = 0;   // 0, 0: fit inside the box ("contain")
  int iw, ih;
  if (s.side_fit == "cover" && imageSize(vfs, &iw, &ih)) zx = zy = std::max(r.w / (float)iw, r.h / (float)ih);
  M5.Display.setClipRect(r.x, r.y, r.w, r.h);
  bool ok = false;
  if (png) ok = M5.Display.drawPngFile((fs::FS &)SD_MMC, fsp.c_str(), r.x, r.y, r.w, r.h, 0, 0, zx, zy, middle_center);
  else if (jpg) ok = M5.Display.drawJpgFile((fs::FS &)SD_MMC, fsp.c_str(), r.x, r.y, r.w, r.h, 0, 0, zx, zy, middle_center);
  M5.Display.clearClipRect();
  if (!ok) note("sign: could not draw side image %s (PNG or baseline JPEG only)", rel.c_str());
}

// ---- quick panel ------------------------------------------------------------
volatile bool g_panel_req = false, g_panel_open = false;
volatile bool g_active = true, g_was_active = true;
volatile int g_tap_x = -1, g_tap_y = -1;
uint32_t g_panel_until = 0;

struct Button {
  int x, y, w, h;
  bool hit(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};
// The info sheet (long press anywhere): the address and a QR code to the web
// page, how the sign is doing, and the controls people reach for. 80 px+
// targets (Tabulous5: ~294 PPI).
const Button kPanel{40, 28, 1200, 664};
const Button kVolDown{700, 128, 110, 96}, kVolUp{1090, 128, 110, 96};
const Button kBriDown{700, 262, 110, 96}, kBriUp{1090, 262, 110, 96};
const Button kModes[4] = {{700, 410, 118, 96}, {828, 410, 118, 96}, {956, 410, 118, 96}, {1084, 410, 116, 96}};
const char *const kModeLabels[4] = {"Messages", "Clock", "Weather", "Photos"};
const char *const kModeSources[4] = {"messages", "clock", "weather", "photos"};
const Button kMute{700, 560, 118, 96}, kNext{828, 560, 118, 96}, kShowIp{956, 560, 118, 96}, kClose{1084, 560, 116, 96};

// Settings changes go to the main loop: this task's stack is in PSRAM and
// must never write flash.
QueueHandle_t g_actions = xQueueCreate(8, sizeof(Action));
volatile uint32_t g_redraw_at = 0;

void post(ActionKind k, int arg) {
  Action a{k, arg};
  xQueueSend(g_actions, &a, 0);
  g_redraw_at = millis() + 350;   // show the result once the main loop has applied it
}

void drawButton(const Button &b, const char *label, uint16_t bg, uint16_t fg, const lgfx::IFont *font) {
  auto &d = M5.Display;
  d.fillRoundRect(b.x, b.y, b.w, b.h, 16, bg);
  d.setFont(font);
  d.setTextColor(fg, bg);
  d.setTextDatum(middle_center);
  d.drawString(label, b.x + b.w / 2, b.y + b.h / 2);
}

// Just the bar between a level's - and + buttons (what a tap changes).
void drawLevelBar(int value, bool on, const Button &down, const Button &up) {
  auto &d = M5.Display;
  const uint16_t ink = 0xF79E, accent = 0xEC20;
  const int bx = down.x + down.w + 18, bw = up.x - 18 - bx, by = down.y + 20, bh = down.h - 40;
  d.fillRoundRect(bx, by, bw, bh, 12, 0x18E3);
  if (on && value > 0) d.fillRoundRect(bx, by, std::max(24, bw * value / 100), bh, 12, accent);
  d.setFont(&lgfx::fonts::FreeSansBold18pt7b);
  d.setTextDatum(middle_center);
  d.setTextColor(ink);
  char t[16];
  snprintf(t, sizeof(t), on ? "%d%%" : "MUTED", value);
  d.drawString(t, bx + bw / 2, by + bh / 2);
}

void drawLevel(const char *label, int value, bool on, const Button &down, const Button &up) {
  auto &d = M5.Display;
  const uint16_t panel = 0x2124, ink = 0xF79E, dim = 0xA534, btn = 0x39C7;
  d.setFont(&lgfx::fonts::FreeSans12pt7b);
  d.setTextDatum(bottom_left);
  d.setTextColor(dim, panel);
  d.drawString(label, down.x, down.y - 8);
  drawButton(down, "-", btn, ink, &lgfx::fonts::FreeSansBold24pt7b);
  drawButton(up, "+", btn, ink, &lgfx::fonts::FreeSansBold24pt7b);
  drawLevelBar(value, on, down, up);
}

// After a tap: repaint only the controls whose state can have changed, not
// the whole sheet (repainting it all made the sheet flash on every press).
struct ShownControls {
  int vol = -1, bri = -1;
  int on = -1;
  std::string source;
} g_shown_ctl;

void drawControlsState() {
  const uint16_t ink = 0xF79E, accent = 0xEC20, btn = 0x39C7;
  int bri;
  std::string source;
  {
    config::Reader r;
    bri = r.doc()["brightness"] | 80;
    source = r.doc()["content_source"] | "messages";
  }
  const int vol = sound::volume(), on = sound::enabled() ? 1 : 0;
  auto &c = g_shown_ctl;
  M5.Display.startWrite();
  if (vol != c.vol || on != c.on) drawLevelBar(vol, on, kVolDown, kVolUp);
  if (bri != c.bri) drawLevelBar(bri, true, kBriDown, kBriUp);
  if (source != c.source)
    for (int i = 0; i < 4; i++)
      if (source == kModeSources[i] || c.source == kModeSources[i])   // only the old and the new choice
        drawButton(kModes[i], kModeLabels[i], source == kModeSources[i] ? accent : btn, ink, &lgfx::fonts::FreeSans12pt7b);
  if (on != c.on) drawButton(kMute, on ? "Mute" : "Unmute", on ? btn : accent, ink, &lgfx::fonts::FreeSans12pt7b);
  M5.Display.endWrite();
  c = {vol, bri, on, source};
}

void drawPanel() {
  auto &d = M5.Display;
  const uint16_t panel = 0x2124, ink = 0xF79E, dim = 0xA534, accent = 0xEC20, btn = 0x39C7, good = 0x5E8B, warn = 0xF5A0;
  JsonDocument st;
  deserializeJson(st, status::json());
  d.startWrite();
  d.fillRoundRect(kPanel.x, kPanel.y, kPanel.w, kPanel.h, 26, panel);
  const int lx = kPanel.x + 44;
  // The address: big, readable from across the room, and a QR code for phones.
  d.setTextDatum(top_left);
  d.setFont(&lgfx::fonts::FreeSansBold24pt7b);
  d.setTextColor(ink, panel);
  d.drawString(config::deviceName().c_str(), lx, kPanel.y + 30);
  const std::string ip = net::ip();
  d.setFont(&lgfx::fonts::FreeSansBold18pt7b);
  d.setTextColor(accent, panel);
  d.drawString((config::hostname() + ".local").c_str(), lx, kPanel.y + 92);
  d.setFont(&lgfx::fonts::FreeSans18pt7b);
  d.setTextColor(ink, panel);
  d.drawString(ip.empty() ? ("Wi-Fi: " + net::statusText()).c_str() : ("http://" + ip + "/").c_str(), lx, kPanel.y + 138);
  if (!ip.empty()) {
    d.fillRect(lx, kPanel.y + 196, 212, 212, TFT_WHITE);
    d.qrcode(("http://" + ip + "/").c_str(), lx + 6, kPanel.y + 202, 200, 3);
  }
  // How it is doing.
  const int ix = lx + 236;
  int y = kPanel.y + 196;
  auto line = [&](const char *k, const std::string &v, uint16_t c) {
    d.setFont(&lgfx::fonts::FreeSans9pt7b);
    d.setTextColor(dim, panel);
    d.drawString(k, ix, y);
    d.setFont(&lgfx::fonts::FreeSans12pt7b);
    d.setTextColor(c, panel);
    std::string t = v.size() > 31 ? v.substr(0, 30) + "..." : v;   // the column ends where the controls begin
    d.drawString(t.c_str(), ix, y + 18);
    y += 50;
  };
  JsonObject w = st["wifi"];
  const bool wifi_ok = std::string(w["state"] | "") == "connected";
  const int rssi = w["rssi"] | 0;
  const uint16_t bad = 0xF9A6;   // a weak signal is the usual reason uploads fail, so say so in colour
  line("WI-FI", wifi_ok ? std::string(w["ssid"] | "") + "  " + std::to_string(rssi) + " dBm" + (rssi < -85 ? "  (weak)" : "")
                        : std::string(w["state"] | "off"), !wifi_ok ? warn : rssi < -85 ? bad : rssi < -75 ? warn : good);
  const bool mq = st["mqtt"]["connected"] | false;
  line("HOME ASSISTANT", mq ? "connected" : std::string(st["mqtt"]["state"] | "off"), mq ? good : dim);
  line("CLOCK", std::string(st["content"]["clock"] | "") + ", " + std::string(st["content"]["timezone"] | ""),
       (st["content"]["clock_trusted"] | false) ? ink : warn);
  line("SHOWING", std::string(st["content"]["source"] | "") + ": " + std::string(st["content"]["reason"] | ""), ink);
  char b[96];
  const uint64_t fb = st["sd"]["free_bytes"] | 0ULL;
  const std::string sdp = st["sd"]["problem"] | "";
  if (st["sd"]["mounted"] | false) snprintf(b, sizeof(b), "ready, %.1f GB free", fb / 1e9);
  else if (sdp == "none") snprintf(b, sizeof(b), "NO CARD (insert one, then restart)");
  else snprintf(b, sizeof(b), "%s: format it from the web page (Files)",
                sdp == "exfat" ? "exFAT, CAN'T READ" : sdp == "ntfs" ? "NTFS, CAN'T READ" : sdp == "unformatted" ? "NOT FORMATTED" : "CAN'T READ");
  line("SD CARD", b, (st["sd"]["mounted"] | false) ? ink : warn);
  const uint32_t up = st["uptime_s"] | 0;
  snprintf(b, sizeof(b), "v%s  chip %s  up %luh%02lum", FLAPBOARD_VERSION, (const char *)(st["wifi_chip"]["firmware"] | "?"),
           (unsigned long)(up / 3600), (unsigned long)(up / 60 % 60));
  line("FIRMWARE", b, dim);
  // The controls.
  drawLevel("VOLUME", sound::volume(), sound::enabled(), kVolDown, kVolUp);
  int bri;
  std::string source;
  {
    config::Reader r;
    bri = r.doc()["brightness"] | 80;
    source = r.doc()["content_source"] | "messages";
  }
  drawLevel("BRIGHTNESS", bri, true, kBriDown, kBriUp);
  d.setFont(&lgfx::fonts::FreeSans12pt7b);
  d.setTextDatum(bottom_left);
  d.setTextColor(dim, panel);
  d.drawString("SHOW", kModes[0].x, kModes[0].y - 8);
  for (int i = 0; i < 4; i++)
    drawButton(kModes[i], kModeLabels[i], source == kModeSources[i] ? accent : btn, ink, &lgfx::fonts::FreeSans12pt7b);
  drawButton(kMute, sound::enabled() ? "Mute" : "Unmute", sound::enabled() ? btn : accent, ink, &lgfx::fonts::FreeSans12pt7b);
  drawButton(kNext, "Next", btn, ink, &lgfx::fonts::FreeSans12pt7b);
  drawButton(kShowIp, "Show IP", btn, ink, &lgfx::fonts::FreeSans12pt7b);
  drawButton(kClose, "Close", btn, ink, &lgfx::fonts::FreeSansBold12pt7b);
  d.endWrite();
  g_shown_ctl = {sound::volume(), bri, sound::enabled() ? 1 : 0, source};
}

// Returns true while the panel is up (the board is not drawn meanwhile).
struct SignState;
void redrawAll(SignState &st);
bool runPanel(SignState &st) {
  if (g_panel_req) {
    g_panel_req = false;
    if (!g_panel_open) {
      g_panel_open = true;
      drawPanel();
    }
    g_panel_until = millis() + 30000;
  }
  if (!g_panel_open) return false;
  const int x = g_tap_x, y = g_tap_y;
  if (x >= 0) {
    g_tap_x = g_tap_y = -1;
    g_panel_until = millis() + 30000;
    bool close = !kPanel.hit(x, y) || kClose.hit(x, y);
    if (kVolDown.hit(x, y)) sound::setVolume(sound::volume() - 10);
    else if (kVolUp.hit(x, y)) sound::setVolume(sound::volume() + 10);
    else if (kMute.hit(x, y)) {
      sound::setEnabled(!sound::enabled());
      if (sound::enabled()) sound::setVolume(sound::volume());   // a preview when unmuting
    } else if (kBriDown.hit(x, y)) post(ActionKind::Brightness, -10);
    else if (kBriUp.hit(x, y)) post(ActionKind::Brightness, +10);
    else if (kNext.hit(x, y)) post(ActionKind::Next, 0);
    else if (kShowIp.hit(x, y)) {
      post(ActionKind::ShowIp, 0);
      close = true;   // so the board can show it
    } else {
      for (int i = 0; i < 4; i++)
        if (kModes[i].hit(x, y)) post(ActionKind::Source, i);
    }
    if (!close) drawControlsState();
    else g_panel_until = 0;
  }
  if (g_redraw_at && (int32_t)(millis() - g_redraw_at) >= 0) {
    g_redraw_at = 0;
    drawControlsState();   // the main loop has applied the change by now
  }
  if ((int32_t)(millis() - g_panel_until) >= 0) {
    g_panel_open = false;
    redrawAll(st);
    return false;
  }
  return true;
}

struct ToMixer : FlipSink {
  void onFlip(const FlipEvent &e) override { sound::flip(e.at_ms); }
} g_flips;

// Serial 'bench': times each part of drawing a cell, 200 times each.
void bench(Renderer &ren, PanelSurface &surf, const GlyphSet &g, const LayoutResult &lay) {
  const int w = lay.cell_w, h = lay.cell_h, n = 200;
  uint16_t *in_ram = (uint16_t *)heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  uint16_t *in_ps = (uint16_t *)heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM);
  auto time = [&](const char *what, auto fn) {
    const int64_t t = esp_timer_get_time();
    for (int i = 0; i < n; i++) fn(i);
    note("bench: %-44s %7.1f us per cell", what, (esp_timer_get_time() - t) / (float)n);
  };
  const size_t bytes = (size_t)w * h * 2;
  time("memcpy face (PSRAM) -> internal RAM", [&](int i) { memcpy(in_ram, g.face(i % g.size()), bytes); });
  time("memcpy face (PSRAM) -> PSRAM buffer", [&](int i) { memcpy(in_ps, g.face(i % g.size()), bytes); });
  time("memcpy same face -> internal (cached)", [&](int) { memcpy(in_ram, g.face(5), bytes); });
  time("memcpy internal -> framebuffer columns", [&](int i) {
    const Rect r = lay.cell(i % 6, (i / 6) % 22);
    int step;
    for (int c = 0; c < w; c++) memcpy(surf.column(r.x + c, r.y, &step), in_ram + (size_t)c * h, (size_t)h * 2);
  });
  time("cache msync of one cell's rows", [&](int i) {
    const Rect r = lay.cell(i % 6, (i / 6) % 22);
    surf.columnsDone(r.x, r.y, r.w, r.h);
  });
  CellView still;
  still.cur = 10;
  time("draw landed cell (full path)", [&](int i) { ren.drawCell(surf, i % 6, (i / 6) % 22, still); });
  CellView mv;
  mv.cur = 10;
  mv.next = 11;
  mv.moving = true;
  mv.progress = 0.3f;
  time("draw mid-flip cell (full path)", [&](int i) {
    ren.invalidate();
    ren.drawCell(surf, i % 6, (i / 6) % 22, mv);
  });
  time("compose mid-flip into scratch (no fb)", [&](int) { ren.compose(mv); });
  heap_caps_free(in_ram);
  heap_caps_free(in_ps);
}
std::string g_pending;
bool g_has_pending = false;
MessageOptions g_pending_opt;

// Frame statistics, reported every 2 s while the board moves.
struct Stats {
  uint32_t frames = 0, cells = 0, max_cells = 0;
  uint64_t draw_us = 0, max_us = 0, blit_us = 0, sync_us = 0;
} g_stats, g_last;


// ---- photo mode ------------------------------------------------------------------
//
// Two full-screen canvases in PSRAM, created in the panel's native (portrait)
// layout with the display's rotation, so a canvas's memory is laid out exactly
// like the framebuffer: showing one is a straight copy (M5GFX keeps sprite
// pixels byte-swapped, hence the bswap). Transitions blend or shift between
// the two straight into panel memory.

enum class Mode { Board, Photo };
volatile Mode g_mode_req = Mode::Board;
std::string g_photo_req, g_caption_req;
volatile bool g_photo_pending = false, g_caption_pending = false;
std::string g_photo_failed;
volatile bool g_photo_failed_flag = false;

struct PhotoState {
  M5Canvas *cur = nullptr, *next = nullptr;
  M5Canvas *tiny = nullptr;   // the blur-fill source
  std::string shown, caption;
  int rot = 3;
  int last_minute = -1;
  Rect overlay{0, 0, 0, 0};   // where text was drawn (restored before redrawing)
  uint32_t decode_ms = 0, transition_ms = 0, frames = 0;
} g_ph;

bool ensureCanvases(int rot) {
  if (g_ph.cur && g_ph.rot == rot) return true;
  for (M5Canvas **c : {&g_ph.cur, &g_ph.next}) {
    if (*c) {
      (*c)->deleteSprite();
      delete *c;
    }
    *c = new M5Canvas(&M5.Display);
    (*c)->setPsram(true);
    (*c)->setColorDepth(16);
    if (!(*c)->createSprite(720, 1280)) return false;
    (*c)->setRotation(rot);
    (*c)->fillScreen(TFT_BLACK);
  }
  if (!g_ph.tiny) {
    g_ph.tiny = new M5Canvas(&M5.Display);
    g_ph.tiny->setPsram(true);
    g_ph.tiny->setColorDepth(16);
    g_ph.tiny->createSprite(64, 36);
  }
  g_ph.rot = rot;
  return true;
}

struct PhotoSettings {
  std::string transition = "dissolve", fit = "contain";
  int transition_ms = 700;
  bool blur = true, clock = false;
  std::string clock_pos = "bottom_right", time_format = "%-I:%M %p";
};

PhotoSettings photoSettings() {
  PhotoSettings p;
  config::Reader r;
  auto &d = r.doc();
  p.transition = d["photo_transition"] | "dissolve";
  p.transition_ms = clampv<int>(d["photo_transition_ms"] | 700, 100, 3000);
  p.fit = d["photo_fit"] | "contain";
  p.blur = d["photo_blur"] | true;
  p.clock = d["photo_clock"] | false;
  p.clock_pos = d["photo_clock_pos"] | "bottom_right";
  p.time_format = d["time_format"] | "%-I:%M %p";
  return p;
}

// Decodes a library photo ("photos/x.jpg") into the canvas: blurred cover
// background, then the photo fitted on top.
bool decodePhoto(M5Canvas &c, const std::string &rel, const PhotoSettings &ps) {
  const std::string vfs = std::string(sdcard::mountPoint()) + "/flapboard/" + rel;
  const std::string fsp = "/flapboard/" + rel;
  std::string lower = rel;
  for (auto &ch : lower) ch = (char)tolower((unsigned char)ch);
  const bool png = lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".png") == 0;
  int iw = 0, ih = 0;
  if (!imageSize(vfs, &iw, &ih)) {
    note("photo: %s: cannot read its size", rel.c_str());
    return false;
  }
  const int W = c.width(), H = c.height();
  auto draw = [&](lgfx::LGFXBase &dst, int x, int y, int w, int h, float zoom) {
    return png ? dst.drawPngFile((fs::FS &)SD_MMC, fsp.c_str(), x, y, w, h, 0, 0, zoom, zoom, middle_center)
               : dst.drawJpgFile((fs::FS &)SD_MMC, fsp.c_str(), x, y, w, h, 0, 0, zoom, zoom, middle_center);
  };
  c.fillScreen(TFT_BLACK);
  const float contain = std::min(W / (float)iw, H / (float)ih), cover = std::max(W / (float)iw, H / (float)ih);
  if (ps.fit == "contain" && ps.blur && std::fabs(contain - cover) > 0.01f) {
    // The Pi frame's blur fill: the photo, tiny and covering, scaled up
    // smoothly (and dimmed) behind the real one instead of black bars.
    auto &t = *g_ph.tiny;
    t.fillScreen(TFT_BLACK);
    if (!draw(t, 0, 0, 64, 36, std::max(64.0f / iw, 36.0f / ih))) note("photo: %s: blur copy failed", rel.c_str());
    // Bilinear enlargement, dimmed to ~55%, written straight into the canvas
    // memory (panel layout, byte-swapped pixels). M5GFX's 20x zoom was blocky.
    const uint16_t *sp = (const uint16_t *)t.getBuffer();
    uint16_t *dp = (uint16_t *)c.getBuffer();
    const int pw = 720, ph = 1280;   // the canvas's native size
    // Integer bilinear: the tiny picture unpacked once, weights per column
    // and row computed once (the float version added 0.8 s per photo).
    static uint8_t ch[3][36][64];
    for (int y = 0; y < 36; y++)
      for (int x = 0; x < 64; x++) {
        const uint16_t v = __builtin_bswap16(sp[y * 64 + x]);
        ch[0][y][x] = (v >> 11) & 31;
        ch[1][y][x] = (v >> 5) & 63;
        ch[2][y][x] = v & 31;
      }
    // Blur the tiny picture itself (three 3x3 box passes ~ a Gaussian): an
    // enlarged 64x36 image otherwise shows its pixels as soft squares.
    static uint8_t tmp[36][64];
    for (int k = 0; k < 3; k++)
      for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < 36; y++)
          for (int x = 0; x < 64; x++) {
            int sum = 0, n = 0;
            for (int dy = -1; dy <= 1; dy++)
              for (int dx = -1; dx <= 1; dx++) {
                const int yy = y + dy, xx = x + dx;
                if (yy < 0 || yy >= 36 || xx < 0 || xx >= 64) continue;
                sum += ch[k][yy][xx];
                n++;
              }
            tmp[y][x] = (uint8_t)(sum / n);
          }
        memcpy(ch[k], tmp, sizeof(tmp));
      }
    static uint8_t x0s[1280], x1s[1280], txs[1280];
    for (int x = 0; x < W; x++) {
      const int f = std::max(0, std::min(63 * 256, ((2 * x + 1) * 64 * 256 / (2 * W)) - 128));
      x0s[x] = f >> 8;
      x1s[x] = std::min(63, (f >> 8) + 1);
      txs[x] = f & 255;
    }
    for (int y = 0; y < H; y++) {
      const int f = std::max(0, std::min(35 * 256, ((2 * y + 1) * 36 * 256 / (2 * H)) - 128));
      const int y0 = f >> 8, y1 = std::min(35, y0 + 1), ty = f & 255;
      for (int x = 0; x < W; x++) {
        const int a0 = x0s[x], a1 = x1s[x], tx = txs[x];
        uint32_t out[3];
        for (int k = 0; k < 3; k++) {
          const int top = ch[k][y0][a0] * (256 - tx) + ch[k][y0][a1] * tx;
          const int bot = ch[k][y1][a0] * (256 - tx) + ch[k][y1][a1] * tx;
          out[k] = (uint32_t)((top * (256 - ty) + bot * ty) >> 16) * 140 >> 8;   // dimmed to ~55%
        }
        const int row = g_ph.rot == 3 ? ph - 1 - x : x, col = g_ph.rot == 3 ? y : pw - 1 - y;
        dp[row * pw + col] = __builtin_bswap16((uint16_t)((out[0] << 11) | (out[1] << 5) | out[2]));
      }
    }
  }
  const bool ok = draw(c, 0, 0, W, H, ps.fit == "cover" ? cover : contain);
  if (!ok)
    note("photo: %s: decoder refused it (%dx%d; internal RAM %u KB, PSRAM largest %u KB)", rel.c_str(), iw, ih,
         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
         (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
  return ok;
}

// The P4's 2D engine: a dissolve frame is one hardware blend of the two
// canvases (byte-swapped inputs, as M5GFX stores sprites) straight into the
// framebuffer. The CPU version managed ~8 frames a second.
ppa_client_handle_t g_ppa = nullptr;
bool g_ppa_failed = false;

bool ppaBlend(PanelSurface &surf, M5Canvas *bg, M5Canvas *fg, int alpha) {
  if (g_ppa_failed) return false;
  if (!g_ppa) {
    ppa_client_config_t c = {};
    c.oper_type = PPA_OPERATION_BLEND;
    if (ppa_register_client(&c, &g_ppa) != ESP_OK) {
      g_ppa_failed = true;
      return false;
    }
  }
  auto in = [&](M5Canvas *cv) {
    ppa_in_pic_blk_config_t b = {};
    b.buffer = cv->getBuffer();
    b.pic_w = surf.pw;
    b.pic_h = surf.ph;
    b.block_w = surf.pw;
    b.block_h = surf.ph;
    b.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;
    return b;
  };
  ppa_blend_oper_config_t o = {};
  o.in_bg = in(bg ? bg : fg);
  o.in_fg = in(fg);
  o.out.buffer = surf.fb;
  o.out.buffer_size = surf.stride * surf.ph;
  o.out.pic_w = surf.pw;
  o.out.pic_h = surf.ph;
  o.out.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;
  o.bg_byte_swap = true;
  o.fg_byte_swap = true;
  o.bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE;
  o.bg_alpha_fix_val = 255;
  o.fg_alpha_update_mode = PPA_ALPHA_FIX_VALUE;
  o.fg_alpha_fix_val = (uint32_t)std::max(0, std::min(255, alpha));
  o.mode = PPA_TRANS_MODE_BLOCKING;
  const esp_err_t e = ppa_do_blend(g_ppa, &o);
  if (e != ESP_OK) {
    note("photo: hardware blend unavailable (%s); using the CPU", esp_err_to_name(e));
    g_ppa_failed = true;
    return false;
  }
  return true;
}

// Canvas -> framebuffer. mix 0..256 blends from `from` (0) to `to` (256);
// shift slides `to` in from the right by that many logical pixels left to go.
void showFrame(PanelSurface &surf, M5Canvas *from, M5Canvas *to, int mix, int shift) {
  uint16_t *fb = (uint16_t *)surf.fb;
  const uint16_t *a = from ? (const uint16_t *)from->getBuffer() : nullptr;
  const uint16_t *b = (const uint16_t *)to->getBuffer();
  const size_t n = (size_t)surf.pw * surf.ph;
  if (shift <= 0 && ppaBlend(surf, from, to, mix >= 256 || !a ? 255 : mix)) return;   // hardware; it syncs the caches
  if (shift > 0 && a) {
    // Logical x runs along panel rows (rotation 3: row = ph-1-x). Rows whose
    // logical x < 1280-shift show `from` moved left by (1280-shift)... i.e.
    // the old picture leaving and the new arriving, row blocks only.
    const int W = surf.ph, sh = shift;
    for (int x = 0; x < W; x++) {
      const int row = surf.rot == 3 ? surf.ph - 1 - x : x;
      const int src_x = x + (W - sh);            // into the old picture, shifted left
      const bool old = src_x < W;
      const int sx = old ? src_x : src_x - W;    // into the new one
      const int srow = surf.rot == 3 ? surf.ph - 1 - sx : sx;
      const uint16_t *src = (old ? a : b) + (size_t)srow * surf.pw;
      uint16_t *dst = fb + (size_t)row * surf.pw;
      for (int i = 0; i < surf.pw; i++) dst[i] = __builtin_bswap16(src[i]);
    }
  } else if (mix >= 256 || !a) {
    for (size_t i = 0; i < n; i++) fb[i] = __builtin_bswap16(b[i]);
  } else {
    const uint32_t k = (uint32_t)mix, j = 256 - k;
    for (size_t i = 0; i < n; i++) {
      const uint16_t p = __builtin_bswap16(a[i]), q = __builtin_bswap16(b[i]);
      const uint32_t rb = (((p & 0xF81Fu) * j + (q & 0xF81Fu) * k) >> 8) & 0xF81Fu;
      const uint32_t g = (((p & 0x07E0u) * j + (q & 0x07E0u) * k) >> 8) & 0x07E0u;
      fb[i] = (uint16_t)(rb | g);
    }
  }
  esp_cache_msync(surf.fb, surf.stride * surf.ph, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

// Put back what the canvas has under a logical rectangle (before redrawing
// the clock or a caption over the photo).
void restoreRect(PanelSurface &surf, const Rect &r) {
  if (r.w <= 0 || !g_ph.cur) return;
  const uint16_t *src = (const uint16_t *)g_ph.cur->getBuffer();
  uint16_t *fb = (uint16_t *)surf.fb;
  for (int x = r.x; x < r.x + r.w; x++) {
    const int row = surf.rot == 3 ? surf.ph - 1 - x : x;
    const int c0 = surf.rot == 3 ? r.y : surf.pw - (r.y + r.h), c1 = c0 + r.h;
    for (int c = std::max(0, c0); c < std::min(surf.pw, c1); c++)
      fb[(size_t)row * surf.pw + c] = __builtin_bswap16(src[(size_t)row * surf.pw + c]);
  }
  esp_cache_msync(surf.fb, surf.stride * surf.ph, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

// The clock and/or a message over the photo, outlined so it reads on any picture.
void drawOverlay(PanelSurface &surf, const PhotoSettings &ps) {
  restoreRect(surf, g_ph.overlay);
  g_ph.overlay = {0, 0, 0, 0};
  std::string text;
  if (!g_ph.caption.empty()) text = g_ph.caption;
  else if (ps.clock) {
    struct tm t;
    if (clock::localNow(&t)) text = flapcore::formatTime(ps.time_format, t);
    g_ph.last_minute = t.tm_min;
  }
  if (text.empty()) return;
  for (auto &ch : text)
    if (ch == '|' || ch == '\n') ch = ' ';
  auto &d = M5.Display;
  const bool caption = !g_ph.caption.empty();
  d.setFont(caption ? &lgfx::fonts::DejaVu40 : &lgfx::fonts::DejaVu72);   // native sizes: no doubled pixels
  d.setTextSize(1);
  const int tw = d.textWidth(text.c_str()), th = d.fontHeight();
  const int W = d.width(), H = d.height(), m = 36;
  int x = W - m - tw, y = H - m - th;
  if (caption) {
    x = (W - tw) / 2;
    y = H - m - th;
  } else if (ps.clock_pos == "bottom_left") {
    x = m;
  } else if (ps.clock_pos == "top_right") {
    y = m;
  } else if (ps.clock_pos == "top_left") {
    x = m;
    y = m;
  }
  g_ph.overlay = {std::max(0, x - 24), std::max(0, y - 16), std::min(W, tw + 48), std::min(H, th + 32)};
  d.startWrite();
  if (caption) d.fillRoundRect(g_ph.overlay.x, g_ph.overlay.y, g_ph.overlay.w, g_ph.overlay.h, 14, 0x0841);
  d.setTextDatum(top_left);
  d.setTextColor(TFT_BLACK);
  for (int dx = -2; dx <= 2; dx += 2)
    for (int dy = -2; dy <= 2; dy += 2)
      if (dx || dy) d.drawString(text.c_str(), x + dx, y + dy);
  d.setTextColor(TFT_WHITE);
  d.drawString(text.c_str(), x, y);
  d.endWrite();
  d.setTextSize(1);
}

// Shows a new photo with the chosen transition. Returns false if it could not be read.
bool presentPhoto(PanelSurface &surf, const std::string &rel, bool first) {
  const PhotoSettings ps = photoSettings();
  if (!ensureCanvases(surf.rot)) return false;
  uint32_t t0 = millis();
  // One retry: the SD card shares the P4's SDIO host with the Wi-Fi chip, and
  // a read during heavy Wi-Fi traffic (just after joining) has failed once.
  if (!decodePhoto(*g_ph.next, rel, ps)) {
    vTaskDelay(pdMS_TO_TICKS(400));
    if (!decodePhoto(*g_ph.next, rel, ps)) return false;
  }
  g_ph.decode_ms = millis() - t0;
  t0 = millis();
  int frames = 0;
  const std::string tr = first ? "cut" : ps.transition;
  if (tr == "dissolve" || tr == "slide") {
    for (;;) {
      const uint32_t e = millis() - t0;
      if (e >= (uint32_t)ps.transition_ms) break;
      const float f = e / (float)ps.transition_ms;
      const float ease = f * f * (3 - 2 * f);
      if (tr == "dissolve") showFrame(surf, g_ph.cur, g_ph.next, (int)(ease * 256), 0);
      else showFrame(surf, g_ph.cur, g_ph.next, 0, std::max(1, (int)(ease * surf.ph)));
      frames++;
      vTaskDelay(1);
    }
  }
  showFrame(surf, nullptr, g_ph.next, 256, 0);
  g_ph.transition_ms = millis() - t0;
  g_ph.frames = frames;
  std::swap(g_ph.cur, g_ph.next);
  g_ph.shown = rel;
  g_ph.overlay = {0, 0, 0, 0};
  drawOverlay(surf, ps);
  note("photo: %s decoded in %lu ms, %s in %lu ms (%d frames)", rel.c_str(), (unsigned long)g_ph.decode_ms, tr.c_str(),
       (unsigned long)g_ph.transition_ms, frames);
  return true;
}

// Everything the board is drawn from, rebuilt when the layout settings change.
struct SignState {
  Drum drum = Drum::vestaboard();
  Settings set;
  std::string key;
  LayoutInput li;
  LayoutResult lay;
  Theme theme;
  TrueTypeFont font;
  GlyphSet glyphs{kPsram};
  Renderer ren{kPsram};
  Board board;
  PanelSurface surf;
  std::string message = "";
  MessageOptions opt;
  bool fits = true;
  size_t glyph_bytes = 0;
};
SignState *g_sign = nullptr;

// The glyph cache holds every drum position at the cell size; huge flaps
// (one row of four, say) would want tens of MB, so cells are capped.
constexpr size_t kGlyphBudget = 6u * 1024 * 1024;

void redrawAll(SignState &st) {
  M5.Display.fillScreen(st.theme.background);
  drawSideImage(st.lay.left, st.set.left, st.set);
  drawSideImage(st.lay.right, st.set.right, st.set);
  st.ren.drawBackground(st.surf);
  st.ren.invalidate();
  st.board.markAllDirty();
}

void rebuild(SignState &st, const Settings &s) {
  const uint32_t t0 = millis();
  st.set = s;
  st.key = s.layoutKey();
  M5.Display.setRotation(s.flipped ? 1 : 3);
  st.surf.begin();
  st.li = LayoutInput();
  st.li.screen_w = M5.Display.width();
  st.li.screen_h = M5.Display.height();
  st.li.rows = s.rows;
  st.li.cols = s.cols;
  st.li.flap_w = s.flap_w;
  st.li.aspect = s.aspect;
  st.li.gap = s.gap;
  st.li.margin = s.margin;
  st.li.left_image = !s.left.empty();
  st.li.right_image = !s.right.empty();
  st.li.side_pct = s.side_pct;
  st.lay = computeLayout(st.li);
  st.fits = st.lay.fits;
  const size_t per_px = 2 * st.drum.size();
  if ((size_t)st.lay.cell_w * st.lay.cell_h * per_px > kGlyphBudget) {
    st.li.flap_w = (int)std::sqrt(kGlyphBudget / (per_px * s.aspect));
    st.lay = computeLayout(st.li);
    note("sign: flaps limited to %d px wide (glyph memory)", st.lay.cell_w);
  }
  st.theme = Theme::custom(s.theme, s.c_bg, s.c_flap, s.c_glyph);
  loadFont(st.font, s.font);
  st.glyphs.build(st.drum, st.theme, st.font, st.lay.cell_w, st.lay.cell_h, s.cap, /*column_major=*/true);
  st.glyph_bytes = st.glyphs.bytes();
  st.ren.setup(st.lay, &st.glyphs, st.theme);
  st.board.resize(s.rows, s.cols, (int)st.drum.size());
  st.board.setMotion(s.motion);
  st.board.jump(layoutMessage(st.drum, st.message, s.rows, s.cols, st.opt));   // the current message, re-flowed
  redrawAll(st);
  note("sign: %dx%d cells of %dx%d px, %s, theme %s, glyphs %u KB, rebuilt in %lu ms%s", s.rows, s.cols,
       st.lay.cell_w, st.lay.cell_h, g_font_used.c_str(), s.theme.c_str(), (unsigned)(st.glyph_bytes / 1024),
       (unsigned long)(millis() - t0), st.fits ? "" : " (fixed flap width too big: shrunk to fit)");
}

void renderTask(void *) {
  SignState &st = *new SignState();
  g_sign = &st;
  rebuild(st, readSettings());

  uint32_t last_report = millis(), last_settings = millis();
  for (;;) {
    const uint32_t now = millis();
    // Settings: a layout change rebuilds; a timing change just applies.
    if (now - last_settings >= 500) {
      last_settings = now;
      const Settings s = readSettings();
      if (s.layoutKey() != st.key) rebuild(st, s);
      else if (s.motion.flip_ms != st.set.motion.flip_ms || s.motion.speed_variance != st.set.motion.speed_variance ||
               s.motion.start != st.set.motion.start) {
        st.set.motion = s.motion;
        st.board.setMotion(s.motion);
      }
    }
    std::string msg;
    MessageOptions opt;
    bool have = false;
    xSemaphoreTake(g_mux, portMAX_DELAY);
    if (g_has_pending) {
      msg = g_pending;
      opt = g_pending_opt;
      have = true;
      g_has_pending = false;
    }
    xSemaphoreGive(g_mux);
    if (have) {
      st.message = msg;
      st.opt = opt;
      st.board.show(layoutMessage(st.drum, msg, st.set.rows, st.set.cols, opt), now);
    }
    if (g_bench) {
      g_bench = false;
      bench(st.ren, st.surf, st.glyphs, st.lay);
      redrawAll(st);
    }
    st.board.update(now, &g_flips, sound::kLookaheadMs);   // landings reported early: clacks land on their sample
    if (!g_active) {   // dark: keep the board's time, draw nothing
      g_was_active = false;
      g_panel_open = false;
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    if (!g_was_active) {
      g_was_active = true;
      if (g_mode_req == Mode::Board) redrawAll(st);
      else if (g_ph.cur) showFrame(st.surf, nullptr, g_ph.cur, 256, 0);
    }
    // Photo mode: the board keeps its time underneath; panel taps still work.
    static Mode mode = Mode::Board;
    if (g_mode_req != mode) {
      mode = g_mode_req;
      if (mode == Mode::Board) {
        g_ph.shown.clear();
        redrawAll(st);
      }
    }
    if (mode == Mode::Photo && !g_panel_open && !g_panel_req) {
      std::string req, cap;
      bool have = false, have_cap = false;
      xSemaphoreTake(g_mux, portMAX_DELAY);
      if (g_photo_pending) {
        req = g_photo_req;
        have = true;
        g_photo_pending = false;
      }
      if (g_caption_pending) {
        cap = g_caption_req;
        have_cap = true;
        g_caption_pending = false;
      }
      xSemaphoreGive(g_mux);
      if (have && !presentPhoto(st.surf, req, g_ph.shown.empty())) {
        note("photo: could not show %s (PNG or baseline JPEG only)", req.c_str());
        xSemaphoreTake(g_mux, portMAX_DELAY);
        g_photo_failed = req;
        g_photo_failed_flag = true;
        xSemaphoreGive(g_mux);
      }
      if (have_cap && cap != g_ph.caption) {
        g_ph.caption = cap;
        drawOverlay(st.surf, photoSettings());
      }
      struct tm t;
      if (g_ph.cur && clock::localNow(&t) && t.tm_min != g_ph.last_minute && g_ph.caption.empty()) {
        const PhotoSettings ps = photoSettings();
        if (ps.clock) drawOverlay(st.surf, ps);
        else g_ph.last_minute = t.tm_min;
      }
      vTaskDelay(pdMS_TO_TICKS(30));
      continue;
    }
    if (mode == Mode::Photo && (g_panel_open || g_panel_req)) {
      runPanel(st);   // the sheet over the photo; closing it restores the photo below
      if (!g_panel_open && g_ph.cur) showFrame(st.surf, nullptr, g_ph.cur, 256, 0), drawOverlay(st.surf, photoSettings());
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    const int64_t a = esp_timer_get_time();
    st.surf.blit_us = st.surf.sync_us = 0;
    int n = 0;
    if (runPanel(st)) {
      // Cells the panel leaves visible keep turning; the covered ones are
      // redrawn when it closes.
      for (int i = 0; i < st.board.cells(); i++) {
        const Rect r = st.lay.cell(i / st.board.cols(), i % st.board.cols());
        const bool covered = r.x < kPanel.x + kPanel.w && r.x + r.w > kPanel.x && r.y < kPanel.y + kPanel.h &&
                             r.y + r.h > kPanel.y;
        if (covered || !st.board.takeDirty(i, now)) continue;
        st.ren.drawCell(st.surf, i / st.board.cols(), i % st.board.cols(), st.board.view(i, now));
        n++;
      }
    } else {
      n = st.ren.drawDirty(st.surf, st.board, now);
    }
    const uint64_t us = (uint64_t)(esp_timer_get_time() - a);
    g_stats.blit_us += st.surf.blit_us;
    g_stats.sync_us += st.surf.sync_us;
    if (n) {
      g_stats.frames++;
      g_stats.cells += n;
      g_stats.draw_us += us;
      if ((uint32_t)n > g_stats.max_cells) g_stats.max_cells = n;
      if (us > g_stats.max_us) g_stats.max_us = us;
    }
    if (now - last_report >= 2000) {
      if (g_stats.frames) {
        trace("sign: %lu frames, %lu cells/frame avg (max %lu), draw %.2f ms avg (max %.2f), per cell %.3f ms",
             (unsigned long)g_stats.frames, (unsigned long)(g_stats.cells / g_stats.frames),
             (unsigned long)g_stats.max_cells, g_stats.draw_us / 1000.0f / g_stats.frames, g_stats.max_us / 1000.0f,
             g_stats.draw_us / 1000.0f / g_stats.cells);
        xSemaphoreTake(g_mux, portMAX_DELAY);
        g_last = g_stats;
        xSemaphoreGive(g_mux);
      }
      g_stats = Stats();
      last_report = now;
    }
    // ~60 Hz target; when nothing moves, just idle.
    const uint32_t spent = millis() - now;
    vTaskDelay(pdMS_TO_TICKS(spent >= 16 ? 1 : 16 - spent));
  }
}

}  // namespace

void begin() {
  g_mux = xSemaphoreCreateMutex();
  // Core 1, above the Arduino loop; its stack in PSRAM (internal RAM feeds Wi-Fi).
  xTaskCreatePinnedToCoreWithCaps(renderTask, "sign", 16384, nullptr, 3, nullptr, 1,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void show(const std::string &text, int align, bool vertical_center) {
  sound::wake();   // start the speaker before the first flap lands
  xSemaphoreTake(g_mux, portMAX_DELAY);
  g_pending = text;
  g_pending_opt = MessageOptions();
  g_pending_opt.align = align == 0 ? Align::Left : align == 2 ? Align::Right : Align::Center;
  g_pending_opt.vertical_center = vertical_center;
  g_has_pending = true;
  xSemaphoreGive(g_mux);
}

void runBench() { g_bench = true; }
void openPanel() { g_panel_req = true; }
bool takeAction(Action *a) { return xQueueReceive(g_actions, a, 0) == pdTRUE; }
const char *sourceForMode(int i) { return i >= 0 && i < 4 ? kModeSources[i] : "messages"; }
void setActive(bool active) { g_active = active; }

void showPhoto(const std::string &rel) {
  xSemaphoreTake(g_mux, portMAX_DELAY);
  g_photo_req = rel;
  g_photo_pending = true;
  g_mode_req = Mode::Photo;
  xSemaphoreGive(g_mux);
}

void showBoard() { g_mode_req = Mode::Board; }
bool photoMode() { return g_mode_req == Mode::Photo; }

void photoCaption(const std::string &text) {
  xSemaphoreTake(g_mux, portMAX_DELAY);
  if (text != g_caption_req || !g_caption_pending) {
    g_caption_req = text;
    g_caption_pending = true;
  }
  xSemaphoreGive(g_mux);
}

bool takePhotoFailure(std::string *rel) {
  xSemaphoreTake(g_mux, portMAX_DELAY);
  const bool f = g_photo_failed_flag;
  if (f) *rel = g_photo_failed;
  g_photo_failed_flag = false;
  xSemaphoreGive(g_mux);
  return f;
}
bool panelOpen() { return g_panel_open; }
void panelTap(int x, int y) {
  g_tap_y = y;
  g_tap_x = x;
}

std::string statsJson() {
  xSemaphoreTake(g_mux, portMAX_DELAY);
  const Stats s = g_last;
  xSemaphoreGive(g_mux);
  std::string extra;
  if (g_sign) {
    char e[200];
    snprintf(e, sizeof(e), ",\"cell_w\":%d,\"cell_h\":%d,\"fits\":%s,\"glyph_kb\":%u,\"font\":\"%s\"",
             g_sign->lay.cell_w, g_sign->lay.cell_h, g_sign->fits ? "true" : "false",
             (unsigned)(g_sign->glyph_bytes / 1024), g_font_used.c_str());
    extra = e;
  }
  char b[160];
  snprintf(b, sizeof(b), "{\"frames\":%lu,\"avg_cells\":%lu,\"max_cells\":%lu,\"avg_ms\":%.2f,\"max_ms\":%.2f",
           (unsigned long)s.frames, (unsigned long)(s.frames ? s.cells / s.frames : 0), (unsigned long)s.max_cells,
           s.frames ? s.draw_us / 1000.0 / s.frames : 0.0, s.max_us / 1000.0);
  return std::string(b) + extra + "}";
}

}  // namespace sign
}  // namespace flapboard
