// The message file format, kept free of Arduino so it is tested on the Mac.
#include <cstdlib>
#include <string>
#include <vector>

#include "content_parse.h"

namespace flapboard {
namespace content {

std::vector<Message> parseFile(const std::string &file, const std::string &text) {
  std::vector<Message> out;
  Message m;
  m.file = file;
  std::vector<std::string> rows;
  auto finish = [&]() {
    if (!rows.empty()) {
      m.text.clear();
      for (size_t i = 0; i < rows.size(); i++) m.text += (i ? "\n" : "") + rows[i];
      out.push_back(m);
    }
    rows.clear();
    m = Message();
    m.file = file;
  };
  size_t p = 0;
  while (p <= text.size()) {
    size_t e = text.find('\n', p);
    if (e == std::string::npos) e = text.size();
    std::string line = text.substr(p, e - p);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    p = e + 1;
    std::string trimmed = line;
    trimmed.erase(0, trimmed.find_first_not_of(" \t"));
    if (trimmed.empty()) {
      finish();
    } else if (trimmed[0] == '#') {
      continue;
    } else if (trimmed[0] == '@' && rows.empty()) {
      // Options line: @left @right @center @top @hold N
      size_t q = 0;
      while (q < trimmed.size()) {
        size_t s = trimmed.find_first_not_of(" \t", q);
        if (s == std::string::npos) break;
        size_t t = trimmed.find_first_of(" \t", s);
        if (t == std::string::npos) t = trimmed.size();
        const std::string w = trimmed.substr(s, t - s);
        if (w == "@left") m.align = 0;
        else if (w == "@right") m.align = 2;
        else if (w == "@center") m.align = 1;
        else if (w == "@top") m.top = true;
        else if (w == "@hold") {
          const size_t ns = trimmed.find_first_not_of(" \t", t);
          if (ns != std::string::npos) {
            m.hold_s = atoi(trimmed.c_str() + ns);
            t = trimmed.find_first_of(" \t", ns);
            if (t == std::string::npos) t = trimmed.size();
          }
        }
        q = t;
      }
    } else if (trimmed == "|") {
      rows.push_back("");   // an empty row inside a message
    } else {
      rows.push_back(line);
    }
    if (e == text.size()) break;
  }
  finish();
  return out;
}

}  // namespace content
}  // namespace flapboard
