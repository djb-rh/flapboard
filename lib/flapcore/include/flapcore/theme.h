// Colours, all RGB565.
#pragma once

#include <cstdint>
#include <string>

namespace flapcore {

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

struct Theme {
  uint16_t background = rgb565(0x0B, 0x0C, 0x0D);   // around and between cells
  uint16_t flap_top = rgb565(0x2A, 0x2D, 0x31);     // upper half of a flap
  uint16_t flap_bottom = rgb565(0x22, 0x25, 0x28);  // lower half (a touch darker, lit from above)
  uint16_t glyph = rgb565(0xF2, 0xEF, 0xE4);
  uint16_t hinge = rgb565(0x05, 0x05, 0x06);         // the split between the halves
  float corner_frac = 0.08f;                         // corner radius / cell width

  static Theme named(const std::string &name);       // "solari" (default), "vesta", "amber", "white"
};

}  // namespace flapcore
