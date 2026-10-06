// The flap wheel: the ordered positions every cell turns through. Flaps only
// ever move forward, so the order decides how long each change takes.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace flapcore {

struct DrumEntry {
  uint32_t cp = 0;        // Unicode code point shown (0 for a colour tile)
  bool tile = false;      // a solid colour flap instead of a character
  char code = 0;          // tile letter used in messages: {R} {O} {Y} {G} {B} {V} {W} {K}
  uint16_t rgb565 = 0;    // tile colour
  // A picture flap instead (tile is also set): one quarter of a 2x2 argyle
  // block, 1 = top left, 2 = top right, 3 = bottom left, 4 = bottom right.
  // Stacked blocks repeat the pattern, for a border down each side.
  uint8_t art = 0;
};

// Tile letters of the argyle quarters, in art order: {a}{b} over {c}{d}.
constexpr char kArgyleCodes[4] = {'a', 'b', 'c', 'd'};

class Drum {
 public:
  // Vestaboard's order: blank, A-Z, 1-9, 0, punctuation, degree sign, the
  // eight colour tiles, then the four argyle quarters. 69 positions.
  static Drum vestaboard();
  // A custom drum from a UTF-8 string of characters (position 0 should be
  // a space) followed by tile letters, e.g. tiles "ROYGBVWK".
  static Drum fromString(const std::string &utf8_chars, const std::string &tile_codes);

  size_t size() const { return entries_.size(); }
  const DrumEntry &at(size_t i) const { return entries_[i]; }
  int indexOfChar(uint32_t cp) const;   // -1 if not on the drum
  int indexOfTile(char code) const;     // -1 if not on the drum
  bool hasLowercase() const;
  int argyle(int quarter) const;          // drum position of argyle quarter 1-4, -1 if absent
  // Forward steps from one position to another (0 if equal).
  int stepsBetween(int from, int to) const {
    const int n = (int)entries_.size();
    return ((to - from) % n + n) % n;
  }

 private:
  std::vector<DrumEntry> entries_;
};

// The standard tile colours (Vestaboard-like hues, tuned for an LCD).
uint16_t tileColour(char code);

// UTF-8 decode helper shared by the message parser: the code point at s[i],
// advancing i. Invalid bytes decode as U+FFFD.
uint32_t nextCodePoint(const std::string &s, size_t &i);

}  // namespace flapcore
