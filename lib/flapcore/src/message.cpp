#include "flapcore/message.h"

namespace flapcore {
namespace {

using Line = std::vector<uint16_t>;

// One source line -> drum positions (before wrapping or alignment).
Line tokenize(const Drum &drum, const std::string &s, bool upper) {
  Line out;
  for (size_t i = 0; i < s.size();) {
    if (s[i] == '{' && i + 2 < s.size() && s[i + 2] == '}') {
      const int t = drum.indexOfTile(s[i + 1]);
      if (t >= 0) {
        out.push_back((uint16_t)t);
        i += 3;
        continue;
      }
    }
    uint32_t cp = nextCodePoint(s, i);
    if (upper && cp >= 'a' && cp <= 'z') cp -= 32;
    const int k = drum.indexOfChar(cp);
    out.push_back((uint16_t)(k < 0 ? 0 : k));
  }
  return out;
}

void trim(Line &l) {
  size_t a = 0, b = l.size();
  while (a < b && l[a] == 0) a++;
  while (b > a && l[b - 1] == 0) b--;
  l = Line(l.begin() + a, l.begin() + b);
}

// Greedy word wrap on blanks; a word longer than a row is split.
std::vector<Line> wrap(const Line &l, int cols) {
  std::vector<Line> out;
  Line cur;
  size_t i = 0;
  while (i < l.size()) {
    size_t j = i;
    while (j < l.size() && l[j] != 0) j++;   // [i, j) is one word
    Line word(l.begin() + i, l.begin() + j);
    while ((int)word.size() > cols) {        // longer than a row: split it
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
      out.push_back(Line(word.begin(), word.begin() + cols));
      word.erase(word.begin(), word.begin() + cols);
    }
    const int need = (int)cur.size() + (cur.empty() ? 0 : 1) + (int)word.size();
    if (need > cols && !cur.empty()) {
      out.push_back(cur);
      cur.clear();
    }
    if (!cur.empty()) cur.push_back(0);
    cur.insert(cur.end(), word.begin(), word.end());
    i = j;
    while (i < l.size() && l[i] == 0) i++;   // collapse the gap between words
  }
  if (!cur.empty() || out.empty()) out.push_back(cur);
  return out;
}

}  // namespace

std::vector<uint16_t> layoutMessage(const Drum &drum, const std::string &text, int rows, int cols,
                                    const MessageOptions &opt) {
  if (opt.argyle_border && cols > 4 && drum.argyle(1) >= 0) {
    MessageOptions inner = opt;
    inner.argyle_border = false;
    const int ic = cols - 4;
    const std::vector<uint16_t> body = layoutMessage(drum, text, rows, ic, inner);
    std::vector<uint16_t> grid((size_t)rows * cols, 0);
    for (int r = 0; r < rows; r++) {
      const int q = (r % 2 == 0) ? 1 : 3;   // a block is two rows: top quarters, then bottom
      const uint16_t left = (uint16_t)drum.argyle(q), right = (uint16_t)drum.argyle(q + 1);
      grid[(size_t)r * cols + 0] = left;
      grid[(size_t)r * cols + 1] = right;
      grid[(size_t)r * cols + cols - 2] = left;
      grid[(size_t)r * cols + cols - 1] = right;
      for (int c = 0; c < ic; c++) grid[(size_t)r * cols + 2 + c] = body[(size_t)r * ic + c];
    }
    return grid;
  }
  const bool upper = !opt.keep_case && !drum.hasLowercase();
  std::vector<Line> lines;
  std::string cur;
  auto flush = [&]() {
    Line l = tokenize(drum, cur, upper);
    // Left keeps the spacing as typed (hand-aligned columns); centred and
    // right-aligned lines are trimmed first.
    if (opt.align != Align::Left) trim(l);
    else while (!l.empty() && l.back() == 0) l.pop_back();
    if (opt.wrap && (int)l.size() > cols) {
      for (auto &w : wrap(l, cols)) lines.push_back(w);
    } else {
      if ((int)l.size() > cols) l.resize(cols);
      lines.push_back(l);
    }
    cur.clear();
  };
  for (char c : text) {
    if (c == '\n' || c == '|') flush();
    else if (c != '\r') cur += c;
  }
  flush();
  // A trailing empty line from "text\n" is not a row.
  while (lines.size() > 1 && lines.back().empty()) lines.pop_back();
  if ((int)lines.size() > rows) lines.resize(rows);

  std::vector<uint16_t> grid((size_t)rows * cols, 0);
  const int top = opt.vertical_center ? (rows - (int)lines.size()) / 2 : 0;
  for (size_t r = 0; r < lines.size(); r++) {
    const Line &l = lines[r];
    const int pad = opt.align == Align::Left ? 0 : opt.align == Align::Right ? cols - (int)l.size()
                                                                              : (cols - (int)l.size()) / 2;
    for (size_t c = 0; c < l.size(); c++) grid[(size_t)(top + r) * cols + pad + c] = l[c];
  }
  return grid;
}

}  // namespace flapcore
