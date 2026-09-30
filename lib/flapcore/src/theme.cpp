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

}  // namespace flapcore
