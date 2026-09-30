// The serial test console (tools/tab5.py drives it). One command a line;
// each ends with "== done <rc>". Prints "ready" once booted.
#pragma once

namespace flapboard {
namespace console {

void begin();
void loop();   // from the main loop

}  // namespace console
}  // namespace flapboard
