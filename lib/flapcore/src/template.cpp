#include "flapcore/template.h"

#include <cctype>
#include <cstdio>

namespace flapcore {
namespace {

const char *const kDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
const char *const kMonths[] = {"January", "February", "March",     "April",   "May",      "June",
                               "July",    "August",   "September", "October", "November", "December"};

std::string num(int v, int width, bool pad) {
  char b[16];
  if (pad) snprintf(b, sizeof(b), "%0*d", width, v);
  else snprintf(b, sizeof(b), "%d", v);
  return b;
}

}  // namespace

std::string formatTime(const std::string &fmt, const struct tm &t) {
  std::string out;
  for (size_t i = 0; i < fmt.size(); i++) {
    if (fmt[i] != '%' || i + 1 >= fmt.size()) {
      out += fmt[i];
      continue;
    }
    bool pad = true;
    char c = fmt[++i];
    if (c == '-' && i + 1 < fmt.size()) {
      pad = false;
      c = fmt[++i];
    }
    const int h12 = t.tm_hour % 12 == 0 ? 12 : t.tm_hour % 12;
    switch (c) {
      case 'H': out += num(t.tm_hour, 2, pad); break;
      case 'I': out += num(h12, 2, pad); break;
      case 'M': out += num(t.tm_min, 2, pad); break;
      case 'S': out += num(t.tm_sec, 2, pad); break;
      case 'p': out += t.tm_hour < 12 ? "AM" : "PM"; break;
      case 'P': out += t.tm_hour < 12 ? "am" : "pm"; break;
      case 'a': out += std::string(kDays[t.tm_wday % 7]).substr(0, 3); break;
      case 'A': out += kDays[t.tm_wday % 7]; break;
      case 'b': out += std::string(kMonths[t.tm_mon % 12]).substr(0, 3); break;
      case 'B': out += kMonths[t.tm_mon % 12]; break;
      case 'd': out += num(t.tm_mday, 2, pad); break;
      case 'e': out += pad ? (t.tm_mday < 10 ? " " : "") + num(t.tm_mday, 1, false) : num(t.tm_mday, 1, false); break;
      case 'm': out += num(t.tm_mon + 1, 2, pad); break;
      case 'y': out += num(t.tm_year % 100, 2, pad); break;
      case 'Y': out += num(t.tm_year + 1900, 4, false); break;
      case 'j': out += num(t.tm_yday + 1, 3, pad); break;
      case '%': out += '%'; break;
      default: out += '%'; out += c;   // unknown: left as written
    }
  }
  return out;
}

std::string expandTemplate(const std::string &tpl, const FieldLookup &lookup) {
  std::string out;
  size_t i = 0;
  while (i < tpl.size()) {
    if (tpl[i] == '{') {
      const size_t close = tpl.find('}', i + 1);
      if (close != std::string::npos) {
        const std::string inner = tpl.substr(i + 1, close - i - 1);
        const size_t colon = inner.find(':');
        const std::string name = inner.substr(0, colon);
        bool word = name.size() >= 2;
        for (char ch : name) word = word && (std::isalnum((unsigned char)ch) || ch == '_');
        std::string value;
        if (word && lookup(name, colon == std::string::npos ? "" : inner.substr(colon + 1), &value)) {
          out += value;
          i = close + 1;
          continue;
        }
      }
    }
    out += tpl[i++];
  }
  return out;
}

}  // namespace flapcore
