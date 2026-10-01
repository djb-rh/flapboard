// Drawing: one cell at any moment of a flip, composed in a cell-sized RGB565
// buffer and handed to the host's Surface as one rectangle.
//
// A flip, seen from the front (theta = progress * 180 degrees):
//   0-90:   the upper half of the current character falls towards you,
//           foreshortened (cos theta) and darkening; behind it the upper half
//           of the next character is revealed.
//   90-180: the lower half of the next character comes down over the lower
//           half of the current one, brightening as it lands.
#pragma once

#include <cstdint>
#include <vector>

#include "flapcore/board.h"
#include "flapcore/glyphs.h"
#include "flapcore/layout.h"
#include "flapcore/theme.h"

namespace flapcore {

class Surface {
 public:
  virtual ~Surface() = default;
  virtual void blit(int x, int y, int w, int h, const uint16_t *rgb565) = 0;
  virtual void fill(int x, int y, int w, int h, uint16_t rgb565) = 0;
  // Optional fast path: a pointer to logical pixel (x, y) when the pixels
  // below it in that column are evenly spaced in memory (null otherwise).
  // *step is +1 (memory runs down the column) or -1 (runs up it, as on a
  // portrait panel turned the other way). With column-major glyphs the
  // renderer then draws straight into it.
  virtual uint16_t *column(int x, int y, int *step) { return nullptr; }
  // Called after a cell was drawn through column(): flush caches etc.
  virtual void columnsDone(int x, int y, int w, int h) {}
};

class Renderer {
 public:
  explicit Renderer(Allocator a = Allocator()) : alloc_(a) {}
  ~Renderer();
  Renderer(const Renderer &) = delete;
  Renderer &operator=(const Renderer &) = delete;

  bool setup(const LayoutResult &layout, const GlyphSet *glyphs, const Theme &theme);
  // Forget what is on screen (after anything else drew over the board).
  void invalidate();
  // The board's surround and gaps (not the cells).
  void drawBackground(Surface &s) const;
  void drawCell(Surface &s, int row, int col, const CellView &v);
  // Every cell that Board::takeDirty says needs it; returns how many.
  int drawDirty(Surface &s, Board &b, uint32_t now_ms);
  // Compose a cell without drawing it (tests, previews). Returns the buffer,
  // in the glyph set's layout (row- or column-major).
  const uint16_t *compose(const CellView &v);

 private:
  Allocator alloc_;
  LayoutResult layout_;
  const GlyphSet *glyphs_ = nullptr;
  Theme theme_;
  uint16_t *scratch_ = nullptr;
  std::vector<uint16_t> map_;   // resampling rows for the falling flap
  // Column-major: compose column c of the cell into out[0..cell_h).
  void composeColumn(const CellView &v, int c, uint16_t *out, int fh, int dark, int shadow, int hinge_lo,
                     int hinge_hi, int inset, int y0, int y1);
  // What each cell showed last frame, so a flap in mid-fall only redraws
  // the half that is changing (the frame budget is PSRAM bandwidth).
  struct Last {
    int16_t cur = -1, next = -1;
    int8_t phase = -1;   // -1 static, 0 upper flap falling, 1 lower flap
  };
  std::vector<Last> last_;
  int cols_ = 0;
  void flipParams(const CellView &v, int *fh, int *dark, int *shadow);
};

}  // namespace flapcore
