#include "sign.h"

#include <Arduino.h>
#include <SD_MMC.h>   // before M5Unified, so M5GFX can draw images from it
#include <M5Unified.h>
#include <esp_cache.h>
#include <esp_heap_caps.h>
#include <lgfx/v1/platforms/esp32p4/Panel_DSI.hpp>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

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

#include "generated/fonts.h"
#include "config.h"
#include "net.h"
#include "note.h"
#include "sdcard.h"
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
volatile int g_tap_x = -1, g_tap_y = -1;
uint32_t g_panel_until = 0;

struct Button {
  int x, y, w, h;
  bool hit(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};
const Button kPanel{240, 110, 800, 500};
const Button kMinus{290, 290, 150, 150}, kPlus{840, 290, 150, 150};
const Button kMute{290, 480, 330, 96}, kClose{660, 480, 330, 96};

void drawButton(const Button &b, const char *label, uint16_t bg, uint16_t fg, const lgfx::IFont *font) {
  auto &d = M5.Display;
  d.fillRoundRect(b.x, b.y, b.w, b.h, 18, bg);
  d.setFont(font);
  d.setTextColor(fg, bg);
  d.setTextDatum(middle_center);
  d.drawString(label, b.x + b.w / 2, b.y + b.h / 2);
}

void drawPanel() {
  auto &d = M5.Display;
  const uint16_t panel = 0x2124, ink = 0xF79E, dim = 0xA534, accent = 0xEC20, btn = 0x39C7;
  d.startWrite();
  d.fillRoundRect(kPanel.x, kPanel.y, kPanel.w, kPanel.h, 26, panel);
  d.setTextDatum(top_left);
  d.setFont(&lgfx::fonts::FreeSansBold18pt7b);
  d.setTextColor(ink, panel);
  const std::string ip = net::ip();
  d.drawString((config::hostname() + ".local").c_str(), kPanel.x + 50, kPanel.y + 36);
  d.setFont(&lgfx::fonts::FreeSans12pt7b);
  d.setTextColor(dim, panel);
  d.drawString((ip.empty() ? std::string("Wi-Fi: ") + net::statusText() : "http://" + ip + "/").c_str(), kPanel.x + 50,
               kPanel.y + 90);
  d.drawString("VOLUME", kPanel.x + 50, kPanel.y + 140);
  drawButton(kMinus, "-", btn, ink, &lgfx::fonts::FreeSansBold24pt7b);
  drawButton(kPlus, "+", btn, ink, &lgfx::fonts::FreeSansBold24pt7b);
  // The level: a bar between the buttons, with the number on it.
  const int bx = kMinus.x + kMinus.w + 40, bw = kPlus.x - 40 - bx, by = kMinus.y + 45, bh = 60;
  const bool on = sound::enabled();
  const int v = sound::volume();
  d.fillRoundRect(bx, by, bw, bh, 12, 0x18E3);
  if (on && v > 0) d.fillRoundRect(bx, by, std::max(24, bw * v / 100), bh, 12, accent);
  d.setFont(&lgfx::fonts::FreeSansBold18pt7b);
  d.setTextDatum(middle_center);
  d.setTextColor(ink);
  char t[16];
  snprintf(t, sizeof(t), on ? "%d%%" : "MUTED", v);
  d.drawString(t, bx + bw / 2, by + bh / 2);
  drawButton(kMute, on ? "Mute" : "Unmute", on ? btn : accent, ink, &lgfx::fonts::FreeSansBold18pt7b);
  drawButton(kClose, "Close", btn, ink, &lgfx::fonts::FreeSansBold18pt7b);
  d.endWrite();
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
    g_panel_until = millis() + 15000;
  }
  if (!g_panel_open) return false;
  const int x = g_tap_x, y = g_tap_y;
  if (x >= 0) {
    g_tap_x = g_tap_y = -1;
    g_panel_until = millis() + 15000;
    bool close = !kPanel.hit(x, y) || kClose.hit(x, y);
    if (kMinus.hit(x, y)) sound::setVolume(sound::volume() - 10);
    else if (kPlus.hit(x, y)) sound::setVolume(sound::volume() + 10);
    else if (kMute.hit(x, y)) {
      sound::setEnabled(!sound::enabled());
      if (sound::enabled()) sound::setVolume(sound::volume());   // a preview when unmuting
    }
    if (!close) drawPanel();
    else g_panel_until = 0;
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
        note("sign: %lu frames, %lu cells/frame avg (max %lu), draw %.2f ms avg (max %.2f), per cell %.3f ms",
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
