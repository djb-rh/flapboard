#include "sign.h"

#include <Arduino.h>
#include <M5Unified.h>
#include <esp_cache.h>
#include <esp_heap_caps.h>
#include <lgfx/v1/platforms/esp32p4/Panel_DSI.hpp>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <algorithm>

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
#include "sound.h"

namespace flapboard {
namespace sign {
namespace {

using namespace flapcore;

void *psramAlloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
const flapcore::Allocator kPsram{psramAlloc, heap_caps_free};

// Straight into the DSI framebuffer. The panel is portrait (720x1280) and the
// sign landscape (rotation 3): logical (x, y) is panel row ph-1-x, column y,
// so each logical column of a cell is one contiguous run in panel memory and
// a blit is a transpose. M5GFX's pushImage rotated pixel by pixel and cost
// ~0.55 ms per cell; this path is measured in the stats line.
struct PanelSurface : Surface {
  uint8_t *fb = nullptr;
  size_t stride = 0;   // bytes per panel row
  int pw = 0, ph = 0;
  uint64_t blit_us = 0, sync_us = 0;

  bool begin() {
    auto *panel = (lgfx::Panel_DSI *)M5.Display.getPanel();
    fb = (uint8_t *)panel->config_detail().buffer;
    pw = panel->config().panel_width;
    ph = panel->config().panel_height;
    stride = ((size_t)pw * 2 + 3) & ~(size_t)3;
    return fb && M5.Display.getRotation() == 3;
  }
  void blit(int x, int y, int w, int h, const uint16_t *px) override {
    const int64_t t = esp_timer_get_time();
    for (int c = 0; c < w; c++) {
      uint16_t *dst = (uint16_t *)(fb + (size_t)(ph - 1 - (x + c)) * stride) + y;
      const uint16_t *src = px + c;
      for (int r = 0; r < h; r++) dst[r] = src[(size_t)r * w];
    }
    const int64_t t2 = esp_timer_get_time();
    // The display DMA reads PSRAM, not the CPU cache: write the rows back.
    uint8_t *lo = fb + (size_t)(ph - (x + w)) * stride;
    const size_t len = (size_t)w * stride;
    esp_cache_msync(lo, len, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    const int64_t t3 = esp_timer_get_time();
    blit_us += (uint64_t)(t3 - t);
    sync_us += (uint64_t)(t3 - t2);
  }
  void fill(int x, int y, int w, int h, uint16_t c) override { M5.Display.fillRect(x, y, w, h, c); }
  // The fast path: flapcore composes each cell column straight into panel
  // memory (glyphs are stored column-major to match), no scratch, no transpose.
  uint16_t *column(int x, int y) override {
    return fb ? (uint16_t *)(fb + (size_t)(ph - 1 - x) * stride) + y : nullptr;
  }
  void columnsDone(int x, int y, int w, int h) override {
    const int64_t t = esp_timer_get_time();
    esp_cache_msync(fb + (size_t)(ph - (x + w)) * stride, (size_t)w * stride,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    sync_us += (uint64_t)(esp_timer_get_time() - t);
  }
};

SemaphoreHandle_t g_mux;
volatile bool g_bench = false;

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
bool runPanel(Board &board, Renderer &ren, PanelSurface &surf, const Theme &theme) {
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
    M5.Display.fillRect(kPanel.x, kPanel.y, kPanel.w, kPanel.h, theme.background);
    ren.drawBackground(surf);
    ren.invalidate();
    board.markAllDirty();
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
    for (int c = 0; c < w; c++) memcpy(surf.column(r.x + c, r.y), in_ram + (size_t)c * h, (size_t)h * 2);
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

// Frame statistics, reported every 2 s while the board moves.
struct Stats {
  uint32_t frames = 0, cells = 0, max_cells = 0;
  uint64_t draw_us = 0, max_us = 0, blit_us = 0, sync_us = 0;
} g_stats, g_last;

// Until Phase 5 brings real content: a few messages on a timer.
const char *const kDemo[] = {
    "WELCOME ABOARD|THE FLAPBOARD EXPRESS",
    "{R}{O}{Y}{G}{B}{V}{R}{O}{Y}{G}{B}{V}{R}{O}{Y}{G}{B}{V}{R}{O}{Y}{G}||DINING CAR OPEN|UNTIL 9:30 PM||{G}{G}{G}{G}{G}{G}{G}{G}{G}{G}{G}{G}{G}{G}{G}{G}{G}{G}{G}{G}{G}{G}",
    "NOW 72° SUNNY|HI 81  LO 64",
    "NEXT STOP|GRAND CENTRAL TERMINAL",
};

void renderTask(void *) {
  const Drum drum = Drum::vestaboard();
  const Theme theme = Theme::named("solari");
  LayoutInput li;
  li.screen_w = M5.Display.width();
  li.screen_h = M5.Display.height();
  const LayoutResult lay = computeLayout(li);

  TrueTypeFont font;
  const fonts::Font *chosen = &fonts::kFonts[0];
  for (const auto &ff : fonts::kFonts)
    if (strcmp(ff.name, "BebasNeue-Regular") == 0) chosen = &ff;
  font.load(chosen->data, chosen->len);

  static GlyphSet glyphs(kPsram);
  const uint32_t t0 = millis();
  glyphs.build(drum, theme, font, lay.cell_w, lay.cell_h, 0.62f, /*column_major=*/true);
  note("sign: %dx%d cells of %dx%d px, font %s, glyphs %u KB built in %lu ms", li.rows, li.cols, lay.cell_w,
       lay.cell_h, chosen->name, (unsigned)(glyphs.bytes() / 1024), (unsigned long)(millis() - t0));
  static Renderer ren(kPsram);
  ren.setup(lay, &glyphs, theme);
  Board board;
  board.resize(li.rows, li.cols, (int)drum.size());
  board.setMotion(Motion());

  PanelSurface surf;
  if (!surf.begin()) note("sign: framebuffer not available (rotation %d)", M5.Display.getRotation());
  M5.Display.fillScreen(theme.background);
  ren.drawBackground(surf);

  size_t demo = 0;
  uint32_t next_demo = millis() + 1500;
  uint32_t last_report = millis();
  for (;;) {
    const uint32_t now = millis();
    std::string msg;
    bool have = false;
    xSemaphoreTake(g_mux, portMAX_DELAY);
    if (g_has_pending) {
      msg = g_pending;
      have = true;
      g_has_pending = false;
    }
    xSemaphoreGive(g_mux);
    if (!have && now >= next_demo && !board.busy(now)) {
      msg = kDemo[demo++ % (sizeof(kDemo) / sizeof(kDemo[0]))];
      have = true;
    }
    if (have) {
      board.show(layoutMessage(drum, msg, li.rows, li.cols), now);
      next_demo = board.finishMs() + 8000;
    }
    if (g_bench) {
      g_bench = false;
      bench(ren, surf, glyphs, lay);
      ren.invalidate();
      for (int i = 0; i < board.cells(); i++) board.takeDirty(i, 0);   // (all redrawn below)
      board.jump(layoutMessage(drum, "BENCH DONE", li.rows, li.cols));
    }
    board.update(now, &g_flips, sound::kLookaheadMs);   // landings reported early: clacks land on their sample
    const int64_t a = esp_timer_get_time();
    surf.blit_us = surf.sync_us = 0;
    int n = 0;
    if (runPanel(board, ren, surf, theme)) {
      // Cells the panel leaves visible keep turning; the covered ones are
      // redrawn when it closes (markAllDirty).
      for (int i = 0; i < board.cells(); i++) {
        const Rect r = lay.cell(i / board.cols(), i % board.cols());
        const bool covered = r.x < kPanel.x + kPanel.w && r.x + r.w > kPanel.x && r.y < kPanel.y + kPanel.h &&
                             r.y + r.h > kPanel.y;
        if (covered || !board.takeDirty(i, now)) continue;
        ren.drawCell(surf, i / board.cols(), i % board.cols(), board.view(i, now));
        n++;
      }
    } else {
      n = ren.drawDirty(surf, board, now);
    }
    const uint64_t us = (uint64_t)(esp_timer_get_time() - a);
    g_stats.blit_us += surf.blit_us;
    g_stats.sync_us += surf.sync_us;
    if (n) {
      g_stats.frames++;
      g_stats.cells += n;
      g_stats.draw_us += us;
      if ((uint32_t)n > g_stats.max_cells) g_stats.max_cells = n;
      if (us > g_stats.max_us) g_stats.max_us = us;
    }
    if (now - last_report >= 2000) {
      if (g_stats.frames) {
        note("sign: %lu frames, %lu cells/frame avg (max %lu), draw %.2f ms avg (max %.2f), of which blit %.2f (cache sync %.2f), per cell %.3f ms",
             (unsigned long)g_stats.frames, (unsigned long)(g_stats.cells / g_stats.frames),
             (unsigned long)g_stats.max_cells, g_stats.draw_us / 1000.0f / g_stats.frames, g_stats.max_us / 1000.0f,
             g_stats.blit_us / 1000.0f / g_stats.frames, g_stats.sync_us / 1000.0f / g_stats.frames, g_stats.draw_us / 1000.0f / g_stats.cells);
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
  M5.Display.setRotation(3);   // landscape, the same way up as Tabulous5's default
  // Core 1, above the Arduino loop; its stack in PSRAM (internal RAM feeds Wi-Fi).
  xTaskCreatePinnedToCoreWithCaps(renderTask, "sign", 16384, nullptr, 3, nullptr, 1,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void show(const std::string &text) {
  xSemaphoreTake(g_mux, portMAX_DELAY);
  g_pending = text;
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
  char b[160];
  snprintf(b, sizeof(b), "{\"frames\":%lu,\"avg_cells\":%lu,\"max_cells\":%lu,\"avg_ms\":%.2f,\"max_ms\":%.2f}",
           (unsigned long)s.frames, (unsigned long)(s.frames ? s.cells / s.frames : 0), (unsigned long)s.max_cells,
           s.frames ? s.draw_us / 1000.0 / s.frames : 0.0, s.max_us / 1000.0);
  return b;
}

}  // namespace sign
}  // namespace flapboard
