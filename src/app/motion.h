// The Tab5's front camera as a motion sensor (PLAN 11a). Frames are reduced
// to an 80x45 brightness grid and compared with a slowly learned background;
// nothing is stored or sent, except the coarse grid on request for the web
// page's aiming view.
#pragma once

#include <cstdint>
#include <string>

namespace flapboard {
namespace motion {

constexpr int kGridW = 80, kGridH = 45;

// In setup(), before Wi-Fi: the camera driver needs contiguous DMA-capable
// internal RAM, and once Wi-Fi is up there is too little left (measured:
// 207 KB largest block before Wi-Fi, 47 KB after -> ESP_ERR_NO_MEM). So it
// starts here when motion sensing is on; switching it on later needs a restart.
void beginEarly();
void loop();                  // main loop: applies the settings
bool needsRestart();          // switched on since boot, camera not running
int state(uint32_t timeout_ms);   // -1 not in use, 1 motion within the timeout, 0 none
bool active();                // motion in the last few seconds (Home Assistant's binary sensor)
void powerChanged();          // the display or lights just switched: re-learn the scene
std::string statusJson();
// The current grid and which cells changed (kGridW*kGridH each); false if off.
bool view(uint8_t *grid, uint8_t *changed);
// One 320x180 RGB picture (3 bytes a pixel, every 4th pixel), taken on
// request for checking the camera against the real scene; never stored.
constexpr int kSnapW = 320, kSnapH = 180;
bool snapshot(uint8_t *out, uint32_t timeout_ms);

}  // namespace motion
}  // namespace flapboard
