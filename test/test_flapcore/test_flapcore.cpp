#include <unity.h>

#include <string>
#include <vector>

#include "flapcore/board.h"
#include "flapcore/clackmixer.h"
#include "flapcore/drum.h"
#include "flapcore/layout.h"
#include "flapcore/message.h"
#include "flapcore/render.h"
#include "flapcore/theme.h"
#include "flapcore/template.h"

using namespace flapcore;

static const Drum kDrum = Drum::vestaboard();

static std::string row(const std::vector<uint16_t> &g, int r, int cols) {
  std::string s;
  for (int c = 0; c < cols; c++) {
    const DrumEntry &e = kDrum.at(g[r * cols + c]);
    s += e.tile ? (char)('a' + (e.code - 'A')) : (e.cp < 128 ? (char)e.cp : '*');
  }
  return s;
}

void test_drum_order() {
  TEST_ASSERT_EQUAL(69, (int)kDrum.size());   // 57 characters + 8 tiles + 4 argyle quarters
  TEST_ASSERT_EQUAL(0, kDrum.indexOfChar(' '));
  TEST_ASSERT_EQUAL(1, kDrum.indexOfChar('A'));
  TEST_ASSERT_EQUAL(27, kDrum.indexOfChar('1'));
  TEST_ASSERT_EQUAL(36, kDrum.indexOfChar('0'));
  TEST_ASSERT_EQUAL(56, kDrum.indexOfChar(0xB0));   // degree sign
  TEST_ASSERT_EQUAL(57, kDrum.indexOfTile('R'));
  TEST_ASSERT_EQUAL(64, kDrum.indexOfTile('K'));
  TEST_ASSERT_EQUAL(-1, kDrum.indexOfChar('a'));
  TEST_ASSERT_FALSE(kDrum.hasLowercase());
  TEST_ASSERT_EQUAL(68, kDrum.stepsBetween(1, 0));   // A back to blank: all the way round
  for (int q = 1; q <= 4; q++) TEST_ASSERT_EQUAL(64 + q, kDrum.argyle(q));   // argyle quarters last
  TEST_ASSERT_EQUAL(65, kDrum.indexOfTile('a'));
}

void test_message_footer_on_bottom_rows() {
  // 6 rows: "HI" centred in the 4 rows above a 2-row footer
  std::string t = "HI";
  t += kFooterMark;
  t += "A|B";
  const auto g = layoutMessage(kDrum, t, 6, 3, MessageOptions());
  TEST_ASSERT_EQUAL(kDrum.indexOfChar('H'), g[1 * 3 + 0]);   // (4 - 1) / 2 = row 1
  TEST_ASSERT_EQUAL(kDrum.indexOfChar('A'), g[4 * 3 + 1]);
  TEST_ASSERT_EQUAL(kDrum.indexOfChar('B'), g[5 * 3 + 1]);
  for (int r : {0, 2, 3}) for (int c = 0; c < 3; c++) TEST_ASSERT_EQUAL(0, g[r * 3 + c]);
}

void test_message_header_and_footer() {
  // 7 rows: header row 0, footer row 6, "HI" centred in rows 1-5 (row 3)
  std::string t = "T";
  t += kHeaderMark;
  t += "HI";
  t += kFooterMark;
  t += "B";
  const auto g = layoutMessage(kDrum, t, 7, 3, MessageOptions());
  TEST_ASSERT_EQUAL(kDrum.indexOfChar('T'), g[0 * 3 + 1]);
  TEST_ASSERT_EQUAL(kDrum.indexOfChar('H'), g[3 * 3 + 0]);
  TEST_ASSERT_EQUAL(kDrum.indexOfChar('B'), g[6 * 3 + 1]);
}

void test_board_redraws_a_run_that_fell_between_frames() {
  // A one-flap run (blank -> A) that starts and lands between two frames
  // must still be drawn: it was showing blank on the screen for good.
  Board b;
  b.resize(1, 1, (int)kDrum.size());
  Motion m;
  m.flip_ms = 70;
  m.speed_variance = 0;
  m.start = StartMode::Together;
  b.setMotion(m);
  b.jump({0});
  TEST_ASSERT_TRUE(b.takeDirty(0, 0));     // the jump
  TEST_ASSERT_FALSE(b.takeDirty(0, 10));   // nothing new
  b.show({1}, 100);
  TEST_ASSERT_TRUE(b.takeDirty(0, 100));   // the new run (not started yet: still blank)
  TEST_ASSERT_TRUE(b.takeDirty(0, 400));   // next frame long after it landed: draw the A
  TEST_ASSERT_EQUAL(1, b.view(0, 400).cur);
  TEST_ASSERT_FALSE(b.takeDirty(0, 420));  // and then it's done
}

void test_message_argyle_border() {
  MessageOptions o;
  o.argyle_border = true;
  const auto g = layoutMessage(kDrum, "HI", 4, 8, o);   // 4 columns of border, 4 for text
  const int a = kDrum.argyle(1), b = kDrum.argyle(2), c = kDrum.argyle(3), d = kDrum.argyle(4);
  for (int r = 0; r < 4; r++) {   // blocks stack: top quarters on even rows, bottom on odd
    const int L = r % 2 == 0 ? a : c, R = r % 2 == 0 ? b : d;
    TEST_ASSERT_EQUAL(L, g[r * 8 + 0]); TEST_ASSERT_EQUAL(R, g[r * 8 + 1]);
    TEST_ASSERT_EQUAL(L, g[r * 8 + 6]); TEST_ASSERT_EQUAL(R, g[r * 8 + 7]);
  }
  // "HI" centred in the 4 text columns of the middle row block
  TEST_ASSERT_EQUAL(kDrum.indexOfChar('H'), g[1 * 8 + 3]);
  TEST_ASSERT_EQUAL(kDrum.indexOfChar('I'), g[1 * 8 + 4]);
}

void test_message_center_wrap_tiles() {
  auto g = layoutMessage(kDrum, "hello|{R}{G} x", 3, 7);
  TEST_ASSERT_EQUAL_STRING(" HELLO ", row(g, 0, 7).c_str());   // 2 lines in 3 rows: (3-2)/2 = no top pad
  g = layoutMessage(kDrum, "hello|{R}{G} x", 4, 7);
  TEST_ASSERT_EQUAL_STRING(" HELLO ", row(g, 1, 7).c_str());
  TEST_ASSERT_EQUAL_STRING(" rg X  ", row(g, 2, 7).c_str());
  g = layoutMessage(kDrum, "THE QUICK BROWN FOX", 3, 9);
  TEST_ASSERT_EQUAL_STRING("THE QUICK", row(g, 0, 9).c_str());
  TEST_ASSERT_EQUAL_STRING("BROWN FOX", row(g, 1, 9).c_str());
  g = layoutMessage(kDrum, "ABCDEFGHIJ", 2, 4);   // a word longer than a row is split
  TEST_ASSERT_EQUAL_STRING("ABCD", row(g, 0, 4).c_str());
  TEST_ASSERT_EQUAL_STRING("EFGH", row(g, 1, 4).c_str());
}

void test_message_left_keeps_spacing_and_unknowns_blank() {
  MessageOptions o;
  o.align = Align::Left;
  o.vertical_center = false;
  auto g = layoutMessage(kDrum, "241   BOS~7:05\nX", 2, 14, o);
  TEST_ASSERT_EQUAL_STRING("241   BOS 7:05", row(g, 0, 14).c_str());
  TEST_ASSERT_EQUAL_STRING("X             ", row(g, 1, 14).c_str());
  o.align = Align::Right;
  g = layoutMessage(kDrum, "72\xC2\xB0", 1, 5, o);   // UTF-8 degree sign
  TEST_ASSERT_EQUAL_STRING("  72*", row(g, 0, 5).c_str());
}

struct Count : FlipSink {
  std::vector<FlipEvent> ev;
  void onFlip(const FlipEvent &e) override { ev.push_back(e); }
};

void test_board_forward_only_constant_speed() {
  Board b;
  b.resize(1, 3, (int)kDrum.size());
  Motion m;
  m.flip_ms = 100;
  m.speed_variance = 0;
  m.start = StartMode::Together;
  b.setMotion(m);
  // A (1 step), C (3 steps), blank->blank (0 steps)
  b.show({1, 3, 0}, 1000);
  TEST_ASSERT_EQUAL_UINT32(1300, b.finishMs());
  Count c;
  b.update(1150, &c);
  TEST_ASSERT_EQUAL(2, (int)c.ev.size());   // cell 0 landed A at 1100, cell 1 landed A at 1100
  TEST_ASSERT_EQUAL_UINT32(1100, c.ev[0].at_ms);
  CellView v = b.view(1, 1150);
  TEST_ASSERT_TRUE(v.moving);
  TEST_ASSERT_EQUAL(1, v.cur);
  TEST_ASSERT_EQUAL(2, v.next);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.5f, v.progress);
  TEST_ASSERT_FALSE(b.view(0, 1150).moving);
  b.update(2000, &c);
  TEST_ASSERT_EQUAL(4, (int)c.ev.size());   // 1 + 3 flaps in all
  TEST_ASSERT_EQUAL(3, b.view(1, 2000).cur);
  TEST_ASSERT_FALSE(b.busy(2000));
}

void test_board_wraps_forward_and_full_spin() {
  Board b;
  b.resize(1, 1, (int)kDrum.size());
  Motion m;
  m.flip_ms = 10;
  m.speed_variance = 0;
  m.start = StartMode::Together;
  b.setMotion(m);
  b.jump({1});                 // showing A
  b.show({0}, 0);              // to blank: 68 flaps forward, never backwards
  TEST_ASSERT_EQUAL_UINT32(680, b.finishMs());
  b.jump({5});
  b.show({5}, 0, true);        // full spin: once round
  TEST_ASSERT_EQUAL_UINT32(690, b.finishMs());
}

void test_board_speed_variance_is_per_module_and_bounded() {
  Board b;
  b.resize(1, 50, (int)kDrum.size());
  Motion m;
  m.flip_ms = 100;
  m.speed_variance = 0.03f;
  m.start = StartMode::Together;
  b.setMotion(m);
  std::vector<uint16_t> t(50, 10);   // everyone 10 flaps
  b.show(t, 0);
  TEST_ASSERT_TRUE(b.finishMs() <= 1030 + 1);
  TEST_ASSERT_TRUE(b.finishMs() >= 1000);
  // Same seed -> same slowest module next time too.
  Board b2;
  b2.resize(1, 50, (int)kDrum.size());
  b2.setMotion(m);
  b2.show(t, 0);
  TEST_ASSERT_EQUAL_UINT32(b.finishMs(), b2.finishMs());
}

void test_board_retarget_mid_flap_keeps_falling_flap() {
  Board b;
  b.resize(1, 1, (int)kDrum.size());
  Motion m;
  m.flip_ms = 100;
  m.speed_variance = 0;
  m.start = StartMode::Together;
  b.setMotion(m);
  b.show({10}, 0);             // blank -> J
  CellView v = b.view(0, 250); // mid third flap: B->C
  TEST_ASSERT_EQUAL(2, v.cur);
  b.show({4}, 250);            // now to D: C, D -> the falling flap (to C) continues
  v = b.view(0, 250);
  TEST_ASSERT_TRUE(v.moving);
  TEST_ASSERT_EQUAL(2, v.cur);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.5f, v.progress);
  TEST_ASSERT_EQUAL_UINT32(400, b.finishMs());   // lands C at 300, D at 400
}

void test_board_dirty_tracking() {
  Board b;
  b.resize(1, 2, (int)kDrum.size());
  Motion m;
  m.flip_ms = 100;
  m.speed_variance = 0;
  m.start = StartMode::Together;
  b.setMotion(m);
  TEST_ASSERT_TRUE(b.takeDirty(0, 0));   // fresh board: draw everything once
  TEST_ASSERT_FALSE(b.takeDirty(0, 0));
  b.show({2, 0}, 0);
  TEST_ASSERT_TRUE(b.takeDirty(0, 50));
  TEST_ASSERT_TRUE(b.takeDirty(0, 150));
  TEST_ASSERT_TRUE(b.takeDirty(0, 250));   // landed: drawn once more
  TEST_ASSERT_FALSE(b.takeDirty(0, 300));
  TEST_ASSERT_TRUE(b.takeDirty(1, 50));    // from resize
  TEST_ASSERT_FALSE(b.takeDirty(1, 60));   // never moves
}

void test_layout_auto_fit_and_sides() {
  LayoutInput in;
  LayoutResult r = computeLayout(in);   // 6x22 on 1280x720
  TEST_ASSERT_TRUE(r.fits);
  TEST_ASSERT_TRUE(r.board.x >= in.margin);
  TEST_ASSERT_TRUE(r.board.x + r.board.w <= 1280 - in.margin);
  TEST_ASSERT_TRUE(r.board.y + r.board.h <= 720 - in.margin);
  TEST_ASSERT_EQUAL(r.cell_w * 22 + 21 * in.gap, r.board.w);
  in.left_image = in.right_image = true;
  LayoutResult s = computeLayout(in);
  TEST_ASSERT_TRUE(s.cell_w < r.cell_w);
  TEST_ASSERT_TRUE(s.left.w > 0 && s.right.w > 0);
  TEST_ASSERT_TRUE(s.left.x + s.left.w <= s.board.x);
  TEST_ASSERT_TRUE(s.board.x + s.board.w <= s.right.x);
  in.left_image = in.right_image = false;
  in.flap_w = 500;   // too big: shrunk to fit, flagged
  LayoutResult f = computeLayout(in);
  TEST_ASSERT_FALSE(f.fits);
  TEST_ASSERT_EQUAL(r.cell_w, f.cell_w);
  in.rows = 3;
  in.cols = 10;
  in.flap_w = 60;    // fixed and small: honoured, centred
  LayoutResult x = computeLayout(in);
  TEST_ASSERT_TRUE(x.fits);
  TEST_ASSERT_EQUAL(60, x.cell_w);
  TEST_ASSERT_EQUAL(84, x.cell_h);
}

// A font without a font file: every character is a filled box whose size
// depends on the code point, so different glyphs really differ.
struct BoxFont : FontRaster {
  int cap = 10;
  void setCapHeight(int c) override { cap = c; }
  bool render(uint32_t cp, GlyphBitmap *g) override {
    g->w = 3 + cp % 7;
    g->h = cap;
    g->top = -cap;
    g->alpha.assign((size_t)g->w * g->h, 0);
    for (int i = 0; i < g->w * g->h; i++) g->alpha[i] = (uint8_t)((i * 37 + cp) & 0xFF);
    return true;
  }
};

struct RowSurface : Surface {
  int W, H;
  std::vector<uint16_t> px;
  RowSurface(int w, int h) : W(w), H(h), px((size_t)w * h, 0) {}
  void blit(int x, int y, int w, int h, const uint16_t *s) override {
    for (int r = 0; r < h; r++)
      for (int c = 0; c < w; c++) px[(size_t)(y + r) * W + x + c] = s[(size_t)r * w + c];
  }
  void fill(int, int, int, int, uint16_t) override {}
};

// Memory laid out column after column, like the Tab5 panel in landscape.
struct ColumnSurface : RowSurface {
  std::vector<uint16_t> cols;
  int done = 0;
  ColumnSurface(int w, int h) : RowSurface(w, h), cols((size_t)w * h, 0) {}
  uint16_t *column(int x, int y, int *step) override {
    *step = 1;
    return &cols[(size_t)x * H + y];
  }
  void columnsDone(int, int, int, int) override { done++; }
  uint16_t at(int x, int y) const { return cols[(size_t)x * H + y]; }
};

// Columns whose memory runs upwards (the Tab5 turned the other way up).
struct ReversedColumnSurface : RowSurface {
  std::vector<uint16_t> cols;
  ReversedColumnSurface(int w, int h) : RowSurface(w, h), cols((size_t)w * h, 0) {}
  uint16_t *column(int x, int y, int *step) override {
    *step = -1;
    return &cols[(size_t)x * H + (H - 1 - y)];
  }
  uint16_t at(int x, int y) const { return cols[(size_t)x * H + (H - 1 - y)]; }
};

void test_column_path_matches_row_path() {
  BoxFont font;
  const Theme theme;
  LayoutInput in;
  in.screen_w = 400;
  in.screen_h = 200;
  in.rows = 2;
  in.cols = 5;
  const LayoutResult lay = computeLayout(in);
  GlyphSet rows_g, cols_g;
  TEST_ASSERT_TRUE(rows_g.build(kDrum, theme, font, lay.cell_w, lay.cell_h));
  TEST_ASSERT_TRUE(cols_g.build(kDrum, theme, font, lay.cell_w, lay.cell_h, 0.62f, true));
  Renderer rr, rc;
  rr.setup(lay, &rows_g, theme);
  rc.setup(lay, &cols_g, theme);
  RowSurface a(400, 200);
  ColumnSurface b(400, 200);
  ReversedColumnSurface c(400, 200);
  const float progress[] = {0.0f, 0.1f, 0.3f, 0.49f, 0.5f, 0.51f, 0.75f, 0.95f};
  for (float p : progress) {
    CellView v;
    v.cur = 5;
    v.next = 6;
    v.moving = p > 0;
    v.progress = p;
    rr.drawCell(a, 1, 3, v);
    rc.drawCell(b, 1, 3, v);
    rc.drawCell(c, 1, 3, v);
    const Rect r = lay.cell(1, 3);
    for (int y = r.y; y < r.y + r.h; y++)
      for (int x = r.x; x < r.x + r.w; x++) {
        if (a.px[(size_t)y * 400 + x] != b.at(x, y) || a.px[(size_t)y * 400 + x] != c.at(x, y)) {
          char m[80];
          snprintf(m, sizeof(m), "progress %.2f differs at cell pixel %d,%d", p, x - r.x, y - r.y);
          TEST_FAIL_MESSAGE(m);
        }
      }
  }
  TEST_ASSERT_EQUAL(8, b.done);
}

void test_board_lookahead_reports_once() {
  Board b;
  b.resize(1, 1, (int)kDrum.size());
  Motion m;
  m.flip_ms = 100;
  m.speed_variance = 0;
  m.start = StartMode::Together;
  b.setMotion(m);
  b.show({10}, 0);
  Count c;
  b.update(0, &c, 150);              // landings at 100 reported early
  TEST_ASSERT_EQUAL(1, (int)c.ev.size());
  TEST_ASSERT_EQUAL_UINT32(100, c.ev[0].at_ms);
  b.update(50, &c, 150);             // nothing new until 200 is within reach
  TEST_ASSERT_EQUAL(2, (int)c.ev.size());   // 200 <= 50+150
  b.show({4}, 150, false);           // retarget mid-flap: the landing at 200 is shared
  b.update(150, &c, 150);            // reach 300: only the new 300 is reported
  TEST_ASSERT_EQUAL(3, (int)c.ev.size());
  TEST_ASSERT_EQUAL_UINT32(300, c.ev[2].at_ms);
  b.update(1000, &c, 150);           // D at 400 (C at 300 was the 3rd)
  TEST_ASSERT_EQUAL(4, (int)c.ev.size());
  TEST_ASSERT_EQUAL_UINT32(400, c.ev[3].at_ms);
}

void test_mixer_places_clacks_on_their_sample() {
  ClackMixer mx;
  mx.setup(1000, 4);   // 1 kHz: one frame per ms
  static int16_t click[10];
  for (int i = 0; i < 10; i++) click[i] = 10000;
  mx.setClips({{click, 10}});
  mx.setVolume(1.0f);
  mx.push(105);
  std::vector<int16_t> out(50);
  mx.render(out.data(), 50, 100.0);   // block covers board time 100..150
  int first = -1;
  for (int i = 0; i < 50; i++)
    if (out[i] != 0) { first = i; break; }
  TEST_ASSERT_EQUAL(5, first);
  auto st = mx.takeStats();
  TEST_ASSERT_EQUAL(1, (int)st.started);
  mx.push(130);                       // the next block: 150..200 -> this one is late
  mx.push(10);                        // far too late: dropped
  mx.push(220);                       // future: kept for a later block
  mx.render(out.data(), 50, 150.0);
  st = mx.takeStats();
  TEST_ASSERT_EQUAL(1, (int)st.started);
  TEST_ASSERT_EQUAL(1, (int)st.dropped);
  TEST_ASSERT_TRUE(out[0] != 0);      // the late one starts immediately
  mx.render(out.data(), 50, 200.0);
  TEST_ASSERT_EQUAL(1, (int)mx.takeStats().started);
  TEST_ASSERT_TRUE(out[19] == 0 && out[21] != 0);   // 220 -> sample 20 (+/- interpolation)
}

void test_mixer_steals_and_never_clips() {
  ClackMixer mx;
  mx.setup(1000, 4);
  static int16_t loud[200];
  for (int i = 0; i < 200; i++) loud[i] = (i & 1) ? 30000 : -30000;
  mx.setClips({{loud, 200}});
  mx.setVolume(1.0f);
  for (int i = 0; i < 10; i++) mx.push(100);   // ten at once, four voices
  std::vector<int16_t> out(100);
  mx.render(out.data(), 100, 100.0);
  const auto st = mx.takeStats();
  TEST_ASSERT_EQUAL(10, (int)st.started);
  TEST_ASSERT_EQUAL(6, (int)st.stolen);
  for (int16_t v : out) TEST_ASSERT_TRUE(v <= 28100 && v >= -28100);
}

void test_theme_custom_colours() {
  uint16_t c = 0;
  TEST_ASSERT_TRUE(Theme::parseHex("#FF8000", &c));
  TEST_ASSERT_EQUAL_HEX16(rgb565(0xFF, 0x80, 0x00), c);
  TEST_ASSERT_FALSE(Theme::parseHex("#12345", &c));
  TEST_ASSERT_FALSE(Theme::parseHex("zz0000", &c));
  const Theme t = Theme::custom("solari", "", "#204060", "#FFFFFF");
  TEST_ASSERT_EQUAL_HEX16(Theme::named("solari").background, t.background);   // empty keeps the preset's
  TEST_ASSERT_EQUAL_HEX16(rgb565(0x20, 0x40, 0x60), t.flap_top);
  TEST_ASSERT_TRUE(t.flap_bottom != t.flap_top);
  TEST_ASSERT_EQUAL_HEX16(rgb565(0xFF, 0xFF, 0xFF), t.glyph);
}

void test_format_time() {
  struct tm t = {};
  t.tm_year = 126; t.tm_mon = 8; t.tm_mday = 3; t.tm_hour = 7; t.tm_min = 5; t.tm_sec = 9; t.tm_wday = 4; t.tm_yday = 245;
  TEST_ASSERT_EQUAL_STRING("7:05 AM", formatTime("%-I:%M %p", t).c_str());
  TEST_ASSERT_EQUAL_STRING("07:05:09", formatTime("%H:%M:%S", t).c_str());
  TEST_ASSERT_EQUAL_STRING("Thu Sep 3", formatTime("%a %b %-d", t).c_str());
  TEST_ASSERT_EQUAL_STRING("Thursday, September 03 2026", formatTime("%A, %B %d %Y", t).c_str());
  TEST_ASSERT_EQUAL_STRING("09/03/26 100%", formatTime("%m/%d/%y 100%%", t).c_str());
  t.tm_hour = 0;
  TEST_ASSERT_EQUAL_STRING("12 am", formatTime("%-I %P", t).c_str());
  t.tm_hour = 12;
  TEST_ASSERT_EQUAL_STRING("12 PM", formatTime("%-I %p", t).c_str());
  TEST_ASSERT_EQUAL_STRING("%Q", formatTime("%Q", t).c_str());
}

void test_expand_template() {
  auto lookup = [](const std::string &n, const std::string &a, std::string *o) {
    if (n == "temp") { *o = "72"; return true; }
    if (n == "time") { *o = a.empty() ? "7:05" : "[" + a + "]"; return true; }
    return false;
  };
  TEST_ASSERT_EQUAL_STRING("NOW 72\xC2\xB0|7:05", expandTemplate("NOW {temp}\xC2\xB0|{time}", lookup).c_str());
  TEST_ASSERT_EQUAL_STRING("[%H:%M]", expandTemplate("{time:%H:%M}", lookup).c_str());
  TEST_ASSERT_EQUAL_STRING("{R}{G} {nope} {x", expandTemplate("{R}{G} {nope} {x", lookup).c_str());
  TEST_ASSERT_EQUAL_STRING("{temp", expandTemplate("{temp", lookup).c_str());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_drum_order);
  RUN_TEST(test_message_center_wrap_tiles);
  RUN_TEST(test_message_argyle_border);
  RUN_TEST(test_board_redraws_a_run_that_fell_between_frames);
  RUN_TEST(test_message_footer_on_bottom_rows);
  RUN_TEST(test_message_header_and_footer);
  RUN_TEST(test_message_left_keeps_spacing_and_unknowns_blank);
  RUN_TEST(test_board_forward_only_constant_speed);
  RUN_TEST(test_board_wraps_forward_and_full_spin);
  RUN_TEST(test_board_speed_variance_is_per_module_and_bounded);
  RUN_TEST(test_board_retarget_mid_flap_keeps_falling_flap);
  RUN_TEST(test_board_dirty_tracking);
  RUN_TEST(test_layout_auto_fit_and_sides);
  RUN_TEST(test_column_path_matches_row_path);
  RUN_TEST(test_board_lookahead_reports_once);
  RUN_TEST(test_mixer_places_clacks_on_their_sample);
  RUN_TEST(test_mixer_steals_and_never_clips);
  RUN_TEST(test_theme_custom_colours);
  RUN_TEST(test_format_time);
  RUN_TEST(test_expand_template);
  return UNITY_END();
}
