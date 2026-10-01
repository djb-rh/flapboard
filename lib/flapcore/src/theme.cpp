#include "flapcore/theme.h"

namespace flapcore {

Theme Theme::named(const std::string &name) {
  Theme t;
  if (name == "vesta") {
    t.background = rgb565(0x14, 0x14, 0x14);
    t.flap_top = rgb565(0x1E, 0x1E, 0x1E);
    t.flap_bottom = rgb565(0x19, 0x19, 0x19);
    t.glyph = rgb565(0xF5, 0xF5, 0xF0);
    t.hinge = rgb565(0x0A, 0x0A, 0x0A);
    t.corner_frac = 0.04f;
  } else if (name == "amber") {
    t.glyph = rgb565(0xFF, 0xB2, 0x3A);
  } else if (name == "white") {
    t.background = rgb565(0xD8, 0xD8, 0xD4);
    t.flap_top = rgb565(0xF4, 0xF3, 0xEE);
    t.flap_bottom = rgb565(0xE8, 0xE7, 0xE1);
    t.glyph = rgb565(0x16, 0x18, 0x1A);
    t.hinge = rgb565(0xB0, 0xB0, 0xAC);
  }
  return t;
}

bool Theme::parseHex(const std::string &hex, uint16_t *out) {
  std::string h = hex;
  if (!h.empty() && h[0] == '#') h.erase(0, 1);
  if (h.size() != 6) return false;
  unsigned v = 0;
  for (char c : h) {
    v <<= 4;
    if (c >= '0' && c <= '9') v |= c - '0';
    else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
    else return false;
  }
  *out = rgb565((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
  return true;
}

namespace {
uint16_t scale(uint16_t p, int k) {   // k/256 brightness
  const int r = ((p >> 11) & 31) * k >> 8, g = ((p >> 5) & 63) * k >> 8, b = (p & 31) * k >> 8;
  return (uint16_t)(((r > 31 ? 31 : r) << 11) | ((g > 63 ? 63 : g) << 5) | (b > 31 ? 31 : b));
}
}  // namespace

Theme Theme::custom(const std::string &preset, const std::string &background, const std::string &flap,
                    const std::string &glyph) {
  Theme t = named(preset);
  uint16_t c;
  if (parseHex(background, &c)) {
    t.background = c;
    t.hinge = scale(c, 120);
  }
  if (parseHex(flap, &c)) {
    t.flap_top = c;
    t.flap_bottom = scale(c, 225);   // lit from above
  }
  if (parseHex(glyph, &c)) t.glyph = c;
  return t;
}

}  // namespace flapcore
