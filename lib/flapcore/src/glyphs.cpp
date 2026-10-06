#include "flapcore/glyphs.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace flapcore {
namespace {

inline uint16_t blend(uint16_t bg, uint16_t fg, int a) {   // a: 0..255
  if (a <= 0) return bg;
  if (a >= 255) return fg;
  const int rb = (((bg >> 11) & 31) * (255 - a) + ((fg >> 11) & 31) * a) / 255;
  const int gb = (((bg >> 5) & 63) * (255 - a) + ((fg >> 5) & 63) * a) / 255;
  const int bb = ((bg & 31) * (255 - a) + (fg & 31) * a) / 255;
  return (uint16_t)((rb << 11) | (gb << 5) | bb);
}

// Coverage of a rounded-rectangle corner at pixel (x, y), 0..255.
int cornerCoverage(int x, int y, int w, int h, float r) {
  if (r < 0.5f) return 255;
  float cx = -1, cy = -1;
  if (x < r) cx = r;
  else if (x >= w - r) cx = w - r;
  if (y < r) cy = r;
  else if (y >= h - r) cy = h - r;
  if (cx < 0 || cy < 0) return 255;
  const float dx = (x + 0.5f) - cx, dy = (y + 0.5f) - cy;
  const float d = std::sqrt(dx * dx + dy * dy) - r;   // <0 inside
  if (d <= -0.5f) return 255;
  if (d >= 0.5f) return 0;
  return (int)((0.5f - d) * 255);
}

}  // namespace

void *Allocator::get(size_t n) const { return alloc ? alloc(n) : std::malloc(n); }
void Allocator::put(void *p) const {
  if (release) release(p);
  else std::free(p);
}

GlyphSet::~GlyphSet() { clear(); }

void GlyphSet::clear() {
  for (auto *f : faces_) alloc_.put(f);
  faces_.clear();
  w_ = h_ = 0;
}

// UNC argyle, one repeat in a 2x2 block of cells: a Carolina-blue diamond
// tip to tip down the block's middle, navy dashes crossing through its centre
// to the block's corners (so they run on into the next block), navy stripes
// down the outer edges, white behind. Drawn per pixel, 3x3 supersampled, in
// block coordinates that include the gap between cells.
uint16_t argylePixel(int quarter, int x, int y, int cw, int ch, int gap) {
  const float W = 2.0f * cw + gap, H = 2.0f * ch + gap;
  const float ox = (quarter == 2 || quarter == 4) ? cw + gap : 0;
  const float oy = (quarter >= 3) ? ch + gap : 0;
  const float stripe = 0.045f * W;                 // navy edge stripes
  const float cx = W / 2, hw = 0.43f * (W - 2 * stripe), hh = H / 2;   // diamond half-width / half-height
  const float lw = std::max(1.2f, 0.022f * W);     // dash line width
  const float dash = 0.07f * H;                    // dash period along the line
  // the two dashed diagonals: (cx-hw, 0)-(cx+hw, H) and (cx+hw, 0)-(cx-hw, H)
  const float len = std::sqrt((2 * hw) * (2 * hw) + H * H);
  int r = 0, g = 0, b = 0;
  for (int sy = 0; sy < 3; sy++)
    for (int sx = 0; sx < 3; sx++) {
      const float X = ox + x + (sx + 0.5f) / 3, Y = oy + y + (sy + 0.5f) / 3;
      int cr = 0xF4, cg = 0xF4, cb = 0xF0;                                     // white
      if (std::fabs(X - cx) / hw + std::fabs(Y - hh) / hh <= 1) cr = 0x7B, cg = 0xAF, cb = 0xD4;   // Carolina blue
      for (int k = 0; k < 2; k++) {
        const float x0 = k == 0 ? cx - hw : cx + hw, x1 = k == 0 ? cx + hw : cx - hw;
        const float dx = x1 - x0, dy = H;
        const float t = ((X - x0) * dx + Y * dy) / (len * len);              // 0..1 along the line
        const float dist = std::fabs((X - x0) * dy - Y * dx) / len;
        if (t >= 0 && t <= 1 && dist <= lw / 2 && std::fmod(t * len, dash) < dash * 0.55f) cr = 0x13, cg = 0x29, cb = 0x4B;
      }
      if (X < stripe || X > W - stripe) cr = 0x13, cg = 0x29, cb = 0x4B;     // navy edge stripes
      r += cr, g += cg, b += cb;
    }
  r /= 9, g /= 9, b /= 9;
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

bool GlyphSet::build(const Drum &drum, const Theme &theme, FontRaster &font, int cw, int ch, float cap_frac,
                     bool column_major, int gap) {
  clear();
  column_major_ = column_major;
  if (cw < 4 || ch < 4) return false;
  w_ = cw;
  h_ = ch;
  const int cap = std::max(3, (int)std::lround(ch * cap_frac));
  font.setCapHeight(cap);
  const float radius = theme.corner_frac * cw;
  const int half = ch / 2;
  // Baseline so that capitals sit centred in the cell.
  const int baseline = (ch + cap) / 2;
  GlyphBitmap g;
  for (size_t i = 0; i < drum.size(); i++) {
    uint16_t *px = (uint16_t *)alloc_.get((size_t)cw * ch * 2);
    if (!px) {
      clear();
      return false;
    }
    faces_.push_back(px);
    const DrumEntry &e = drum.at(i);
    // The flap card: two shades, rounded corners on the board colour.
    for (int y = 0; y < ch; y++) {
      const uint16_t card = e.tile ? e.rgb565 : (y < half ? theme.flap_top : theme.flap_bottom);
      for (int x = 0; x < cw; x++) {
        const uint16_t c = e.art ? argylePixel(e.art, x, y, cw, ch, gap) : card;
        px[y * cw + x] = blend(theme.background, c, cornerCoverage(x, y, cw, ch, radius));
      }
    }
    if (e.tile || e.cp == ' ' || e.cp == 0) continue;
    if (!font.render(e.cp, &g) || g.w <= 0 || g.h <= 0) continue;
    // Too wide for the card (a W in a wide font): shrink this one glyph.
    int gw = g.w, gh = g.h;
    float s = 1.0f;
    if (gw > cw * 0.9f) s = cw * 0.9f / gw;
    const int dw = std::max(1, (int)(gw * s)), dh = std::max(1, (int)(gh * s));
    const int ox = (cw - dw) / 2;
    const int oy = baseline + (int)std::lround(g.top * s);
    for (int y = 0; y < dh; y++) {
      const int py = oy + y;
      if (py < 0 || py >= ch) continue;
      const int sy = std::min(gh - 1, (int)(y / s));
      for (int x = 0; x < dw; x++) {
        const int pxx = ox + x;
        if (pxx < 0 || pxx >= cw) continue;
        const int a = g.alpha[(size_t)sy * gw + std::min(gw - 1, (int)(x / s))];
        if (a) px[py * cw + pxx] = blend(px[py * cw + pxx], theme.glyph, a);
      }
    }
  }
  if (column_major) {   // transpose every face in place, via one temporary
    std::vector<uint16_t> tmp((size_t)cw * ch);
    for (auto *px : faces_) {
      for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++) tmp[(size_t)x * ch + y] = px[(size_t)y * cw + x];
      std::copy(tmp.begin(), tmp.end(), px);
    }
  }
  return true;
}

}  // namespace flapcore
