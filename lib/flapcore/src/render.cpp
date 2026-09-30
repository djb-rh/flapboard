#include "flapcore/render.h"

#include <cmath>
#include <cstring>

namespace flapcore {
namespace {

// Scale an RGB565 pixel's brightness by k/256 (k <= 256): red and blue in
// one multiply, green in another, masked back into place. Three unpacked
// multiplies here made a mid-flip cell cost 222 us on the Tab5.
inline uint16_t shade(uint16_t p, uint32_t k) {
  const uint32_t rb = ((p & 0xF81Fu) * k >> 8) & 0xF81Fu;
  const uint32_t g = ((p & 0x07E0u) * k >> 8) & 0x07E0u;
  return (uint16_t)(rb | g);
}

}  // namespace

Renderer::~Renderer() {
  if (scratch_) alloc_.put(scratch_);
}

bool Renderer::setup(const LayoutResult &layout, const GlyphSet *glyphs, const Theme &theme) {
  layout_ = layout;
  glyphs_ = glyphs;
  theme_ = theme;
  if (scratch_) alloc_.put(scratch_);
  scratch_ = (uint16_t *)alloc_.get((size_t)layout.cell_w * layout.cell_h * 2);
  map_.assign((size_t)layout.cell_h, 0);
  invalidate();
  return scratch_ && glyphs && glyphs->cellW() == layout.cell_w && glyphs->cellH() == layout.cell_h;
}

void Renderer::invalidate() { last_.clear(); }

void Renderer::drawBackground(Surface &s) const {
  const Rect &b = layout_.board;
  s.fill(b.x, b.y, b.w, b.h, theme_.background);
}

// The falling flap's height (negative: the lower flap, below the split),
// its brightness and the shadow it casts, for this moment of the flip.
void Renderer::flipParams(const CellView &v, int *fh, int *dark, int *shadow) {
  const int h = layout_.cell_h, h1 = h / 2, h2 = h - h1;
  *fh = 0;
  *dark = *shadow = 256;
  if (!v.moving || v.progress <= 0.0f) return;
  const float theta = v.progress * 3.14159265f;
  const float c = std::cos(theta), sn = std::sin(theta);
  *dark = 256 - (int)(150 * sn);   // a flap edge-on to the light is darkest
  if (c > 0) {
    *fh = (int)std::lround(h1 * c);
    *shadow = 256 - (int)(70 * c);
    for (int y = 0; y < *fh; y++) map_[y] = y * h1 / *fh;            // rows of the upper half
  } else {
    const int f = (int)std::lround(h2 * -c);
    *fh = -f;
    *shadow = 256 - (int)(60 * (1 + c));
    for (int y = 0; y < f; y++) map_[y] = h1 + y * h2 / f;           // rows of the lower half
  }
}

// One column of a cell (column-major faces): the same composition as the
// row path below, on a contiguous run.
void Renderer::composeColumn(const CellView &v, int col, uint16_t *out, int fh, int dark, int shadow, int hinge_lo,
                             int hinge_hi, int inset, int y0, int y1) {
  const int w = layout_.cell_w, h = layout_.cell_h, h1 = h / 2;
  const uint16_t *cur = glyphs_->face(v.cur) + (size_t)col * h;
  auto lo = [&](int a) { return a < y0 ? y0 : a; };
  auto hi = [&](int b) { return b > y1 ? y1 : b; };
  auto copy = [&](const uint16_t *src, int a, int b) {
    a = lo(a), b = hi(b);
    if (b > a) memcpy(out + a, src + a, (size_t)(b - a) * 2);
  };
  auto shaded = [&](const uint16_t *src, int a, int b, int k) {
    for (int y = lo(a), e = hi(b); y < e; y++) out[y] = shade(src[y], k);
  };
  if (!v.moving || v.progress <= 0.0f) {
    copy(cur, 0, h);
  } else {
    const uint16_t *nxt = glyphs_->face(v.next) + (size_t)col * h;
    if (fh > 0) {   // upper flap falling: next top revealed above it (in its shadow)
      shaded(nxt, 0, h1 - fh, shadow);
      for (int y = lo(h1 - fh), e = hi(h1); y < e; y++) out[y] = shade(cur[map_[y - (h1 - fh)]], dark);
      copy(cur, h1, h);
    } else {        // lower flap coming down over the current bottom
      const int f = -fh;
      copy(nxt, 0, h1);
      for (int y = lo(h1), e = hi(h1 + f); y < e; y++) out[y] = shade(nxt[map_[y - h1]], dark);
      shaded(cur, h1 + f, h, shadow);
    }
  }
  if (col >= inset && col < w - inset)
    for (int y = lo(hinge_lo), e = hi(hinge_hi); y < e; y++) out[y] = theme_.hinge;
}

const uint16_t *Renderer::compose(const CellView &v) {
  const int w = layout_.cell_w, h = layout_.cell_h, h1 = h / 2;
  int fh, dark, shadow;
  flipParams(v, &fh, &dark, &shadow);
  const int line = h >= 90 ? 2 : 1, inset = (int)(theme_.corner_frac * w * 0.3f);
  const int hinge_lo = h1 - line / 2, hinge_hi = hinge_lo + line;
  uint16_t *buf = scratch_;
  if (glyphs_->columnMajor()) {
    for (int c = 0; c < w; c++)
      composeColumn(v, c, buf + (size_t)c * h, fh, dark, shadow, hinge_lo, hinge_hi, inset, 0, h);
    return buf;
  }
  const uint16_t *cur = glyphs_->face(v.cur), *nxt = glyphs_->face(v.next);
  auto row = [&](const uint16_t *face, int y) { return face + (size_t)y * w; };
  for (int y = 0; y < h; y++) {
    uint16_t *dst = buf + (size_t)y * w;
    const uint16_t *src;
    int k = 256;
    if (!v.moving || v.progress <= 0.0f) src = row(cur, y);
    else if (fh > 0) {
      if (y < h1 - fh) { src = row(nxt, y); k = shadow; }
      else if (y < h1) { src = row(cur, map_[y - (h1 - fh)]); k = dark; }
      else src = row(cur, y);
    } else {
      const int f = -fh;
      if (y < h1) src = row(nxt, y);
      else if (y < h1 + f) { src = row(nxt, map_[y - h1]); k = dark; }
      else { src = row(cur, y); k = shadow; }
    }
    if (k >= 256) memcpy(dst, src, (size_t)w * 2);
    else for (int x = 0; x < w; x++) dst[x] = shade(src[x], k);
    if (y >= hinge_lo && y < hinge_hi)
      for (int x = inset; x < w - inset; x++) dst[x] = theme_.hinge;
  }
  return buf;
}

void Renderer::drawCell(Surface &s, int row, int col, const CellView &v) {
  const Rect r = layout_.cell(row, col);
  if (glyphs_->columnMajor() && s.column(r.x, r.y)) {
    const int h = layout_.cell_h, h1 = h / 2;
    int fh, dark, shadow;
    flipParams(v, &fh, &dark, &shadow);
    const int line = h >= 90 ? 2 : 1, inset = (int)(theme_.corner_frac * r.w * 0.3f);
    const int hinge_lo = h1 - line / 2, hinge_hi = hinge_lo + line;
    // Same flap, same half as last frame: only that half changes.
    const int idx = row * cols_ + col;
    const int8_t phase = (!v.moving || v.progress <= 0.0f) ? -1 : (fh > 0 ? 0 : 1);
    int y0 = 0, y1 = h;
    if (idx < (int)last_.size()) {
      const Last &l = last_[idx];
      if (phase >= 0 && l.phase == phase && l.cur == v.cur && l.next == v.next) {
        if (phase == 0) y1 = hinge_hi;
        else y0 = hinge_lo;
      }
    }
    for (int c = 0; c < r.w; c++)
      composeColumn(v, c, s.column(r.x + c, r.y), fh, dark, shadow, hinge_lo, hinge_hi, inset, y0, y1);
    s.columnsDone(r.x, r.y, r.w, r.h);
    if (idx < (int)last_.size()) last_[idx] = {(int16_t)v.cur, (int16_t)v.next, phase};
    return;
  }
  const uint16_t *buf = compose(v);
  if (!glyphs_->columnMajor()) {
    s.blit(r.x, r.y, r.w, r.h, buf);
    return;
  }
  // Column-major glyphs on a surface without columns: transpose for blit.
  static thread_local std::vector<uint16_t> t;
  t.resize((size_t)r.w * r.h);
  for (int x = 0; x < r.w; x++)
    for (int y = 0; y < r.h; y++) t[(size_t)y * r.w + x] = buf[(size_t)x * r.h + y];
  s.blit(r.x, r.y, r.w, r.h, t.data());
}

int Renderer::drawDirty(Surface &s, Board &b, uint32_t now_ms) {
  if ((int)last_.size() != b.cells()) last_.assign((size_t)b.cells(), Last());
  cols_ = b.cols();
  int n = 0;
  for (int i = 0; i < b.cells(); i++) {
    if (!b.takeDirty(i, now_ms)) continue;
    drawCell(s, i / b.cols(), i % b.cols(), b.view(i, now_ms));
    n++;
  }
  return n;
}

}  // namespace flapcore
