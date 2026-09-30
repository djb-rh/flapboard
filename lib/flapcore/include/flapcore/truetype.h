// A FontRaster over stb_truetype. The font data must stay valid while this
// object is used (embed it, or keep the file's bytes in memory).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "flapcore/glyphs.h"

namespace flapcore {

class TrueTypeFont : public FontRaster {
 public:
  TrueTypeFont();
  ~TrueTypeFont() override;
  bool load(const uint8_t *data, size_t size);
  void setCapHeight(int cap_px) override;
  bool render(uint32_t cp, GlyphBitmap *out) override;
  bool hasGlyph(uint32_t cp) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace flapcore
