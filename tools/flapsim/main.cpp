// flapsim: renders a split-flap message sequence through flapcore on the Mac
// and writes an MP4 (via ffmpeg) with a clack mixed in at every flap landing,
// so the look, timing and sound can be judged without flashing the Tab5.
//
//   build/flapsim [options] "MESSAGE ONE" "MESSAGE|TWO" ...
//     --out FILE.mp4     (default build/flapsim.mp4)
//     --font FILE.ttf    (default assets/fonts/BebasNeue-Regular.ttf)
//     --rows N --cols N  (default 6 x 22)
//     --flip MS          time per flap (default 70)
//     --theme NAME       solari | vesta | amber | white
//     --sounds GLOB-PREFIX  e.g. sounds/generated/solari_ (loads _1.._6.wav)
//     --align left|center|right   --hold SECONDS (after each change, default 2)
//     --side             reserve side-image areas (drawn as grey placeholders)
//     --png FILE         also write the last frame as a PNG (needs ffmpeg)
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "flapcore/board.h"
#include "flapcore/drum.h"
#include "flapcore/glyphs.h"
#include "flapcore/layout.h"
#include "flapcore/message.h"
#include "flapcore/render.h"
#include "flapcore/theme.h"
#include "flapcore/truetype.h"

using namespace flapcore;

namespace {

constexpr int W = 1280, H = 720, FPS = 30, RATE = 44100;

struct MemSurface : Surface {
  std::vector<uint16_t> px = std::vector<uint16_t>((size_t)W * H, 0);
  void blit(int x, int y, int w, int h, const uint16_t *s) override {
    for (int r = 0; r < h; r++)
      if (y + r >= 0 && y + r < H) memcpy(&px[(size_t)(y + r) * W + x], s + (size_t)r * w, (size_t)w * 2);
  }
  void fill(int x, int y, int w, int h, uint16_t c) override {
    for (int r = y; r < y + h && r < H; r++)
      for (int q = x; q < x + w && q < W; q++) px[(size_t)r * W + q] = c;
  }
};

std::vector<uint8_t> readFile(const std::string &p) {
  std::vector<uint8_t> d;
  FILE *f = fopen(p.c_str(), "rb");
  if (!f) return d;
  fseek(f, 0, SEEK_END);
  d.resize(ftell(f));
  fseek(f, 0, SEEK_SET);
  if (fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
  fclose(f);
  return d;
}

// 16-bit mono PCM from a WAV (the ones tools/make_clacks.py writes).
std::vector<float> readWav(const std::string &p) {
  std::vector<float> out;
  auto d = readFile(p);
  if (d.size() < 44) return out;
  size_t i = 12;
  while (i + 8 <= d.size()) {
    const uint32_t len = d[i + 4] | d[i + 5] << 8 | d[i + 6] << 16 | (uint32_t)d[i + 7] << 24;
    if (!memcmp(&d[i], "data", 4)) {
      for (size_t k = i + 8; k + 1 < i + 8 + len && k + 1 < d.size(); k += 2)
        out.push_back((int16_t)(d[k] | d[k + 1] << 8) / 32768.0f);
      break;
    }
    i += 8 + len + (len & 1);
  }
  return out;
}

void writeWav(const std::string &p, const std::vector<float> &s) {
  FILE *f = fopen(p.c_str(), "wb");
  auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
  auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
  fwrite("RIFF", 1, 4, f); u32(36 + (uint32_t)s.size() * 2); fwrite("WAVEfmt ", 1, 8, f);
  u32(16); u16(1); u16(1); u32(RATE); u32(RATE * 2); u16(2); u16(16);
  fwrite("data", 1, 4, f); u32((uint32_t)s.size() * 2);
  for (float v : s) u16((uint16_t)(int16_t)std::lround(std::fmax(-1, std::fmin(1, v)) * 32767));
  fclose(f);
}

struct Mixer : FlipSink {
  std::vector<std::vector<float>> clacks;
  std::vector<float> out;
  std::mt19937 rng{7};
  int events = 0;
  void onFlip(const FlipEvent &e) override {
    events++;
    if (clacks.empty()) return;
    const auto &c = clacks[rng() % clacks.size()];
    const float g = 0.18f * (0.55f + (rng() % 1000) / 2222.0f);
    const size_t at = (size_t)e.at_ms * RATE / 1000;
    if (out.size() < at + c.size()) out.resize(at + c.size(), 0);
    for (size_t k = 0; k < c.size(); k++) out[at + k] += c[k] * g;
  }
};

}  // namespace

// --layout-grid: the C++ layout for a fixed grid of inputs, one JSON object a
// line; tools/check_layout.mjs runs web/layout.js over the same grid.
static int layoutGrid() {
  for (int rows = 1; rows <= 12; rows += 1)
    for (int cols = 1; cols <= 40; cols += 3)
      for (int flap : {0, 40, 500})
        for (float aspect : {1.0f, 1.4f, 1.75f})
          for (int gap : {0, 4, 10})
            for (int sides = 0; sides < 3; sides++)
              for (int pct : {10, 25}) {
                LayoutInput in;
                in.rows = rows; in.cols = cols; in.flap_w = flap; in.aspect = aspect; in.gap = gap;
                in.left_image = sides >= 1; in.right_image = sides == 2; in.side_pct = pct;
                const LayoutResult r = computeLayout(in);
                printf("{\"cw\":%d,\"ch\":%d,\"bx\":%d,\"by\":%d,\"bw\":%d,\"bh\":%d,\"lw\":%d,\"rx\":%d,\"rw\":%d,\"fits\":%d}\n",
                       r.cell_w, r.cell_h, r.board.x, r.board.y, r.board.w, r.board.h, r.left.w, r.right.x, r.right.w, r.fits);
              }
  return 0;
}

int main(int argc, char **argv) {
  if (argc > 1 && std::string(argv[1]) == "--layout-grid") return layoutGrid();
  std::string out = "build/flapsim.mp4", font_path = "assets/fonts/BebasNeue-Regular.ttf", theme_name = "solari";
  std::string sounds = "sounds/generated/solari_", png;
  int rows = 6, cols = 22;
  float flip = 70, hold = 2.0f;
  bool side = false;
  MessageOptions mo;
  std::vector<std::string> msgs;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
    if (a == "--out") out = next();
    else if (a == "--font") font_path = next();
    else if (a == "--rows") rows = atoi(next().c_str());
    else if (a == "--cols") cols = atoi(next().c_str());
    else if (a == "--flip") flip = (float)atof(next().c_str());
    else if (a == "--theme") theme_name = next();
    else if (a == "--sounds") sounds = next();
    else if (a == "--hold") hold = (float)atof(next().c_str());
    else if (a == "--side") side = true;
    else if (a == "--png") png = next();
    else if (a == "--align") {
      const std::string v = next();
      mo.align = v == "left" ? Align::Left : v == "right" ? Align::Right : Align::Center;
    } else msgs.push_back(a);
  }
  if (msgs.empty()) msgs = {"WELCOME ABOARD", "NEXT STOP|GRAND CENTRAL"};

  auto font_data = readFile(font_path);
  TrueTypeFont font;
  if (font_data.empty() || !font.load(font_data.data(), font_data.size())) {
    fprintf(stderr, "cannot load font %s\n", font_path.c_str());
    return 1;
  }
  const Drum drum = Drum::vestaboard();
  const Theme theme = Theme::named(theme_name);
  LayoutInput li;
  li.rows = rows;
  li.cols = cols;
  li.left_image = li.right_image = side;
  const LayoutResult lay = computeLayout(li);
  GlyphSet glyphs;
  if (!glyphs.build(drum, theme, font, lay.cell_w, lay.cell_h)) return 1;
  Renderer ren;
  ren.setup(lay, &glyphs, theme);
  Board board;
  board.resize(rows, cols, (int)drum.size());
  Motion m;
  m.flip_ms = flip;
  board.setMotion(m);
  fprintf(stderr, "cell %dx%d, glyph cache %zu KB, board at %d,%d %dx%d\n", lay.cell_w, lay.cell_h,
          glyphs.bytes() / 1024, lay.board.x, lay.board.y, lay.board.w, lay.board.h);

  Mixer mix;
  for (int k = 1; k <= 12; k++) {
    auto s = readWav(sounds + std::to_string(k) + ".wav");
    if (s.empty()) break;
    mix.clacks.push_back(s);
  }

  MemSurface surf;
  surf.fill(0, 0, W, H, theme.background);
  if (side) {
    surf.fill(lay.left.x, lay.left.y, lay.left.w, lay.left.h, rgb565(0x3A, 0x3F, 0x44));
    surf.fill(lay.right.x, lay.right.y, lay.right.w, lay.right.h, rgb565(0x3A, 0x3F, 0x44));
  }
  ren.drawBackground(surf);

  const std::string video_tmp = out + ".video.mp4", wav_tmp = out + ".wav";
  const std::string cmd = "ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgb565le -s " + std::to_string(W) + "x" +
                          std::to_string(H) + " -r " + std::to_string(FPS) +
                          " -i - -c:v libx264 -pix_fmt yuv420p -crf 18 '" + video_tmp + "'";
  FILE *ff = popen(cmd.c_str(), "w");
  if (!ff) return 1;

  uint32_t t = 0;
  size_t next_msg = 0;
  uint32_t next_change = 500;
  int frames = 0, max_dirty = 0;
  uint32_t end_ms = 0;
  for (;; t = (uint32_t)(++frames * 1000 / FPS)) {
    if (next_msg < msgs.size() && t >= next_change) {
      board.show(layoutMessage(drum, msgs[next_msg], rows, cols, mo), t);
      next_msg++;
      next_change = board.finishMs() + (uint32_t)(hold * 1000);
      end_ms = next_change;
    }
    board.update(t, &mix);
    const int n = ren.drawDirty(surf, board, t);
    if (n > max_dirty) max_dirty = n;
    fwrite(surf.px.data(), 2, surf.px.size(), ff);
    if (next_msg >= msgs.size() && t >= end_ms) break;
  }
  pclose(ff);
  mix.out.resize((size_t)t * RATE / 1000 + RATE / 2, 0);
  writeWav(wav_tmp, mix.out);
  const std::string mux = "ffmpeg -loglevel error -y -i '" + video_tmp + "' -i '" + wav_tmp +
                          "' -c:v copy -c:a aac -b:a 160k -shortest '" + out + "'";
  if (system(mux.c_str()) != 0) return 1;
  remove(video_tmp.c_str());
  if (!png.empty()) {
    FILE *p = popen(("ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgb565le -s 1280x720 -i - '" + png + "'").c_str(), "w");
    fwrite(surf.px.data(), 2, surf.px.size(), p);
    pclose(p);
  }
  fprintf(stderr, "%d frames (%.1f s), %d flaps, at most %d cells redrawn in a frame -> %s\n", frames,
          frames / (float)FPS, mix.events, max_dirty, out.c_str());
  return 0;
}
