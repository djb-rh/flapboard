// One message file -> its messages (see content.h for the format).
#pragma once

#include <string>
#include <vector>

namespace flapboard {
namespace content {

struct Message {
  std::string file, text;
  int align = 1;    // 0 left, 1 centre, 2 right
  bool top = false;
  int hold_s = 0;   // 0 = the program's dwell
};

std::vector<Message> parseFile(const std::string &file, const std::string &text);

}  // namespace content
}  // namespace flapboard
