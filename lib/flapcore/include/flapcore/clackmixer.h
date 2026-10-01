// Flap sounds: flip events in, PCM out. No audio driver here -- the host
// calls render() from whatever feeds its speaker.
//
// Events carry the board time of the landing; the host tells render() which
// board time the start of each block corresponds to, so every clack lands on
// its own sample. Boards can report landings ahead of time (Board::update's
// lookahead), which is what makes that possible.
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace flapcore {

struct Clip {
  const int16_t *pcm = nullptr;   // mono, at the mixer's rate
  uint32_t frames = 0;
};

class ClackMixer {
 public:
  void setup(uint32_t rate, int voices = 32);
  void setClips(const std::vector<Clip> &clips);
  void setVolume(float v);          // 0..1
  void setOffsetMs(float ms) { offset_ms_ = ms; }   // shift sound later (+) or earlier (-) than the picture

  // Producer side (one thread, e.g. the render task): queue a landing.
  void push(uint32_t at_ms);

  // Consumer side (the audio task): n frames starting at board time
  // block_start_ms. Writes every sample of out.
  void render(int16_t *out, int n, double block_start_ms);

  // Stats since the last call: clacks started, voices stolen, dropped (late).
  struct Stats { uint32_t started = 0, stolen = 0, dropped = 0; int peak_voices = 0; };
  Stats takeStats();

 private:
  struct Voice {
    int clip = -1;
    uint32_t pos = 0;     // 16.16 fixed point into the clip
    uint32_t step = 0;    // 16.16 (pitch)
    int32_t gain = 0;     // Q8
    int start = 0;        // first sample of this block it plays at
  };
  uint32_t rng();
  void start(int offset);

  uint32_t rate_ = 22050;
  std::vector<Voice> voices_;
  std::vector<Clip> clips_;
  std::vector<double> pending_;     // board times not yet reached
  std::vector<int32_t> acc_;
  float volume_ = 0.6f, offset_ms_ = 0;
  float limit_ = 1.0f;              // limiter gain
  uint32_t rng_ = 0x2545F491;
  Stats stats_;

  static constexpr int kRing = 2048;
  uint32_t ring_[kRing];
  std::atomic<uint32_t> head_{0}, tail_{0};
};

}  // namespace flapcore
