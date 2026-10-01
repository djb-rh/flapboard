// Fill-in fields in messages: "{time}", "{date:%a %b %-d}", "{temp}".
// A field name is two or more letters, digits or underscores, so the
// one-letter colour tiles ({R}, {G} ...) are never touched. Fields the
// lookup doesn't know stay as written.
#pragma once

#include <ctime>
#include <functional>
#include <string>

namespace flapcore {

// name, optional argument (after ':') -> value; false if unknown.
using FieldLookup = std::function<bool(const std::string &name, const std::string &arg, std::string *out)>;

std::string expandTemplate(const std::string &tpl, const FieldLookup &lookup);

// strftime's common subset, plus the "-" flag that newlib lacks (%-I, %-d,
// %-m, %-H): %H %I %M %S %p %P %a %A %b %B %d %e %m %y %Y %j %% and %-X.
std::string formatTime(const std::string &fmt, const struct tm &t);

}  // namespace flapcore
