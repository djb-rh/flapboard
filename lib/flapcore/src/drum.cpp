#include "flapcore/drum.h"

namespace flapcore {
namespace {

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

}  // namespace

uint16_t tileColour(char code) {
  switch (code) {
    case 'R': return rgb(0xD2, 0x2F, 0x27);
    case 'O': return rgb(0xF0, 0x7A, 0x1E);
    case 'Y': return rgb(0xF2, 0xC2, 0x2B);
    case 'G': return rgb(0x1E, 0x96, 0x4B);
    case 'B': return rgb(0x1E, 0x7F, 0xCB);
    case 'V': return rgb(0x7B, 0x3F, 0xA0);
    case 'W': return rgb(0xEE, 0xEC, 0xE4);
    case 'K': return rgb(0x08, 0x08, 0x08);
  }
  return 0;
}

uint32_t nextCodePoint(const std::string &s, size_t &i) {
  const unsigned char c = (unsigned char)s[i++];
  if (c < 0x80) return c;
  int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
  if (extra < 0) return 0xFFFD;
  uint32_t cp = c & (0x3F >> extra);
  for (int k = 0; k < extra; k++) {
    if (i >= s.size() || ((unsigned char)s[i] & 0xC0) != 0x80) return 0xFFFD;
    cp = (cp << 6) | ((unsigned char)s[i++] & 0x3F);
  }
  return cp;
}

Drum Drum::fromString(const std::string &chars, const std::string &tiles) {
  Drum d;
  for (size_t i = 0; i < chars.size();) {
    DrumEntry e;
    e.cp = nextCodePoint(chars, i);
    d.entries_.push_back(e);
  }
  for (char t : tiles) {
    DrumEntry e;
    e.tile = true;
    e.code = t;
    e.rgb565 = tileColour(t);
    d.entries_.push_back(e);
  }
  return d;
}

Drum Drum::vestaboard() {
  // Vestaboard's character codes 0-62 in order (its unused codes skipped),
  // then codes 63-70, the colour tiles.
  return fromString(" ABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890!@#$()-+&=;:'\"%,./?°", "ROYGBVWK");
}

int Drum::indexOfChar(uint32_t cp) const {
  for (size_t i = 0; i < entries_.size(); i++)
    if (!entries_[i].tile && entries_[i].cp == cp) return (int)i;
  return -1;
}

int Drum::indexOfTile(char code) const {
  for (size_t i = 0; i < entries_.size(); i++)
    if (entries_[i].tile && entries_[i].code == code) return (int)i;
  return -1;
}

bool Drum::hasLowercase() const {
  for (auto &e : entries_)
    if (!e.tile && e.cp >= 'a' && e.cp <= 'z') return true;
  return false;
}

}  // namespace flapcore
