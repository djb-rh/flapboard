// Every drum position pre-rendered at one cell size, split into the upper
// and lower halves a flap is made of. Built once per layout; drawing a flip
// is then only copies and row resampling.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "flapcore/drum.h"
#include "flapcore/theme.h"

namespace flapcore {

// Where big buffers come from (PSRAM on an ESP32). Defaults to malloc/free.
struct Allocator {
  void *(*alloc)(size_t) = nullptr;
  void (*release)(void *) = nullptr;
  void *get(size_t n) const;
  void put(void *p) const;
};

// An 8-bit coverage bitmap of one character, as a font rasteriser gives it.
struct GlyphBitmap {
  int w = 0, h = 0;
  int left = 0;           // x of the bitmap's left edge relative to the pen
  int top = 0;            // y of the bitmap's top relative to the baseline (negative = above)
  int advance = 0;
  std::vector<uint8_t> alpha;
};

class FontRaster {
 public:
  virtual ~FontRaster() = default;
  // Scale so that capital letters are cap_px tall.
  virtual void setCapHeight(int cap_px) = 0;
  virtual bool render(uint32_t cp, GlyphBitmap *out) = 0;
};

class GlyphSet {
 public:
  explicit GlyphSet(Allocator a = Allocator()) : alloc_(a) {}
  ~GlyphSet();
  GlyphSet(const GlyphSet &) = delete;
  GlyphSet &operator=(const GlyphSet &) = delete;

  // cap_frac: capital height as a fraction of the cell height.
  // column_major: store each face transposed (column after column), for
  // Surfaces whose memory runs down the logical columns -- a portrait panel
  // shown in landscape, like the Tab5's.
  // gap: the space between cells, so picture flaps that span cells (the
  // argyle block) line up across it.
  bool build(const Drum &drum, const Theme &theme, FontRaster &font, int cell_w, int cell_h, float cap_frac = 0.62f,
             bool column_major = false, int gap = 0);
  void clear();

  int cellW() const { return w_; }
  int cellH() const { return h_; }
  int topH() const { return h_ / 2; }
  int bottomH() const { return h_ - h_ / 2; }
  // The full face of drum position i (cell_w x cell_h RGB565), row after row
  // (or column after column when columnMajor()).
  const uint16_t *face(size_t i) const { return faces_[i]; }
  bool columnMajor() const { return column_major_; }
  size_t size() const { return faces_.size(); }
  size_t bytes() const { return faces_.size() * (size_t)w_ * h_ * 2; }

 private:
  Allocator alloc_;
  std::vector<uint16_t *> faces_;
  int w_ = 0, h_ = 0;
  bool column_major_ = false;
};

}  // namespace flapcore
