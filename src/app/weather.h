// Weather from Open-Meteo (free, no key) for the configured latitude and
// longitude, fetched every few minutes by its own task. The last good
// reading is kept, so a failed fetch never blanks the sign.
#pragma once

#include <string>

namespace flapboard {
namespace weather {

void begin();
// Message fields: temp feels hi lo hum wind cond place units wind_units.
bool field(const std::string &name, std::string *out);
std::string statusJson();
void refreshNow();

}  // namespace weather
}  // namespace flapboard
