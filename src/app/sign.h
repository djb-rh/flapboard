// The split-flap board on the Tab5: flapcore drawing into the panel from its
// own task on core 1. Other tasks hand it messages; it never blocks them.
#pragma once

#include <string>

namespace flapboard {
namespace sign {

void begin();                              // after M5.begin(); starts the render task
void show(const std::string &text);        // any task: queue a message
std::string statsJson();                   // last frame timings, for /api/status
void runBench();                           // serial 'bench': time the drawing steps

}  // namespace sign
}  // namespace flapboard
