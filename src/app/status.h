// A snapshot of the device's state for the web page and the info screens.
// Anything on the I2C bus (RTC, battery gauge) is read by the main loop only
// -- two tasks on that bus is how the Tab5's codec and touch chips get
// wedged -- and other tasks read the cached copy.
#pragma once

#include <string>

namespace flapboard {
namespace status {

void update();        // main loop, every couple of seconds
std::string json();   // any task
std::string rtcText();

}  // namespace status
}  // namespace flapboard
