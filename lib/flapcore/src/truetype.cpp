#include "flapcore/truetype.h"

#include <cstdlib>
#include <cstring>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "../third_party/stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace flapcore {

struct TrueTypeFont::Impl {
  stbtt_fontinfo info;
  bool ok = false;
  float scale = 1.0f;
};

TrueTypeFont::TrueTypeFont() : impl_(new Impl()) {}
TrueTypeFont::~TrueTypeFont() = default;

bool TrueTypeFont::load(const uint8_t *data, size_t size) {
  (void)size;
  impl_->ok = stbtt_InitFont(&impl_->info, data, stbtt_GetFontOffsetForIndex(data, 0)) != 0;
  return impl_->ok;
}

void TrueTypeFont::setCapHeight(int cap_px) {
  if (!impl_->ok) return;
  // Measure the font's own capital H rather than trusting OS/2 metrics.
  int x0, y0, x1, y1;
  const int g = stbtt_FindGlyphIndex(&impl_->info, 'H');
  if (g && stbtt_GetGlyphBox(&impl_->info, g, &x0, &y0, &x1, &y1) && y1 > y0) impl_->scale = (float)cap_px / (y1 - y0);
  else impl_->scale = stbtt_ScaleForPixelHeight(&impl_->info, cap_px * 1.4f);
}

bool TrueTypeFont::hasGlyph(uint32_t cp) const { return impl_->ok && stbtt_FindGlyphIndex(&impl_->info, (int)cp) != 0; }

bool TrueTypeFont::render(uint32_t cp, GlyphBitmap *out) {
  if (!impl_->ok) return false;
  const int g = stbtt_FindGlyphIndex(&impl_->info, (int)cp);
  if (!g) return false;
  int x0, y0, x1, y1;
  stbtt_GetGlyphBitmapBox(&impl_->info, g, impl_->scale, impl_->scale, &x0, &y0, &x1, &y1);
  out->w = x1 - x0;
  out->h = y1 - y0;
  out->left = x0;
  out->top = y0;   // stb's y grows downwards from the baseline
  int adv, lsb;
  stbtt_GetGlyphHMetrics(&impl_->info, g, &adv, &lsb);
  out->advance = (int)(adv * impl_->scale);
  out->alpha.assign((size_t)(out->w > 0 ? out->w : 0) * (out->h > 0 ? out->h : 0), 0);
  if (out->w <= 0 || out->h <= 0) return true;
  stbtt_MakeGlyphBitmap(&impl_->info, out->alpha.data(), out->w, out->h, out->w, impl_->scale, impl_->scale, g);
  return true;
}

}  // namespace flapcore
