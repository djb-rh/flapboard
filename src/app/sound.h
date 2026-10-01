// The board's sound on the Tab5: flapcore's ClackMixer fed by the render
// task's flip events, played through M5Unified's speaker by its own task.
#pragma once

#include <cstdint>
#include <string>

namespace flapboard {
namespace sound {

void begin();                     // after M5.begin() and config::begin()
// From the main loop: saves a changed volume to the settings. Flash writes
// must come from a task whose stack is in internal RAM -- the audio and
// render tasks keep theirs in PSRAM, and a LittleFS write from one of them
// asserts (esp_task_stack_is_sane_cache_disabled) and resets the board.
void loop();
void flip(uint32_t at_ms);        // the render task: a flap lands at board time at_ms
void reloadClips();
// Volume 0-100 and on/off, applied at once and saved (debounced) to the
// settings. preview: play a short burst so the level can be heard.
void setVolume(int volume, bool preview = true);
void setEnabled(bool enabled);
int volume();
bool enabled();               // pick up /flapboard/sounds/clack_*.wav again
std::string statsJson();
// Records the next `seconds` of mixer output to path (a WAV), for checking
// timing without ears. Returns at once; a note says when it is written.
void capture(const std::string &path, float seconds);

// How far ahead of a landing the render task should report it: enough to
// cover a slow frame plus the speaker's queue.
constexpr uint32_t kLookaheadMs = 80;

}  // namespace sound
}  // namespace flapboard
