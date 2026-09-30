// Text -> what each cell should show.
//
// Syntax: lines are separated by a newline or '|'. {R} {O} {Y} {G} {B} {V}
// {W} {K} are colour tiles. Characters the drum lacks show as blank; letters
// are upper-cased unless the drum has lowercase (or keep_case is set).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "flapcore/drum.h"

namespace flapcore {

enum class Align { Left, Center, Right };

struct MessageOptions {
  Align align = Align::Center;
  bool vertical_center = true;
  bool wrap = true;          // break long lines at spaces onto the next row
  bool keep_case = false;    // don't upper-case (only useful with a lowercase drum)
};

// rows*cols drum positions, row-major.
std::vector<uint16_t> layoutMessage(const Drum &drum, const std::string &text, int rows, int cols,
                                    const MessageOptions &opt = MessageOptions());

}  // namespace flapcore
