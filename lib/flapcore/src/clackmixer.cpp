#include "flapcore/clackmixer.h"

#include <algorithm>
#include <cmath>

namespace flapcore {

uint32_t ClackMixer::rng() {
  uint32_t x = rng_;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return rng_ = x;
}

void ClackMixer::setup(uint32_t rate, int voices) {
  rate_ = rate;
  voices_.assign((size_t)std::max(1, voices), Voice());
  pending_.clear();
  pending_.reserve(512);
}

void ClackMixer::setClips(const std::vector<Clip> &clips) { clips_ = clips; }
void ClackMixer::setVolume(float v) { volume_ = std::clamp(v, 0.0f, 1.0f); }

void ClackMixer::push(uint32_t at_ms) {
  const uint32_t h = head_.load(std::memory_order_relaxed);
  const uint32_t next = (h + 1) % kRing;
  if (next == tail_.load(std::memory_order_acquire)) return;   // full: drop rather than block the renderer
  ring_[h] = at_ms;
  head_.store(next, std::memory_order_release);
}

ClackMixer::Stats ClackMixer::takeStats() {
  Stats s = stats_;
  stats_ = Stats();
  return s;
}

void ClackMixer::start(int offset) {
  if (clips_.empty()) return;
  // A free voice, or else the one nearest its end: in a full-board rattle a
  // tail nobody can hear any more is the right thing to cut.
  int best = -1;
  uint32_t best_left = UINT32_MAX;
  for (size_t i = 0; i < voices_.size(); i++) {
    const Voice &v = voices_[i];
    if (v.clip < 0) {
      best = (int)i;
      best_left = 0;
      break;
    }
    const uint32_t left = clips_[v.clip].frames - (v.pos >> 16);
    if (left < best_left) {
      best_left = left;
      best = (int)i;
    }
  }
  if (voices_[best].clip >= 0) stats_.stolen++;
  Voice &v = voices_[best];
  v.clip = (int)(rng() % clips_.size());
  v.pos = 0;
  v.step = 65536 * 96 / 100 + rng() % (65536 * 8 / 100);   // 0.96..1.04
  v.gain = 140 + (int32_t)(rng() % 117);                   // ~0.55..1.0
  v.start = offset;
  stats_.started++;
}

void ClackMixer::render(int16_t *out, int n, double block_start_ms) {
  if ((int)acc_.size() < n) acc_.assign((size_t)n, 0);
  std::fill(acc_.begin(), acc_.begin() + n, 0);
  const double ms_per_frame = 1000.0 / rate_;
  const double block_end_ms = block_start_ms + n * ms_per_frame;

  // New events from the renderer.
  uint32_t t = tail_.load(std::memory_order_relaxed);
  const uint32_t h = head_.load(std::memory_order_acquire);
  while (t != h) {
    pending_.push_back(ring_[t] + (double)offset_ms_);
    t = (t + 1) % kRing;
  }
  tail_.store(t, std::memory_order_release);

  // Start every clack that falls in this block, at its own sample. One that
  // is already late plays at once if only a little late, else is dropped (a
  // clack 100 ms after its flap reads as a fault, not as part of the rattle).
  size_t keep = 0;
  for (size_t i = 0; i < pending_.size(); i++) {
    const double at = pending_[i];
    if (at >= block_end_ms) {
      pending_[keep++] = at;
      continue;
    }
    if (at < block_start_ms - 100.0) {
      stats_.dropped++;
      continue;
    }
    const int off = at <= block_start_ms ? 0 : (int)((at - block_start_ms) / ms_per_frame);
    start(std::min(off, n - 1));
  }
  pending_.resize(keep);

  int active = 0;
  for (Voice &v : voices_) {
    if (v.clip < 0) continue;
    active++;
    const Clip &c = clips_[v.clip];
    for (int i = v.start; i < n; i++) {
      const uint32_t idx = v.pos >> 16;
      if (idx + 1 >= c.frames) {
        v.clip = -1;
        break;
      }
      const int32_t frac = (int32_t)(v.pos & 0xFFFF) >> 1;   // 15 bits
      const int32_t s = c.pcm[idx] + (((c.pcm[idx + 1] - c.pcm[idx]) * frac) >> 15);
      acc_[i] += (s * v.gain) >> 8;
      v.pos += v.step;
    }
    v.start = 0;
  }
  stats_.peak_voices = std::max(stats_.peak_voices, active);

  // Volume, then a limiter: fast attack, slow release, so twenty clacks at
  // once get louder but never clip.
  const float vol = volume_ * volume_;   // closer to how loudness is heard
  const float release = 1.0f - 1.0f / (rate_ * 0.25f);
  for (int i = 0; i < n; i++) {
    float x = acc_[i] * vol;
    const float a = std::fabs(x) * limit_;
    if (a > 28000.0f) limit_ = 28000.0f / std::fabs(x);
    else limit_ = std::min(1.0f, limit_ / release);
    x *= limit_;
    if (x > 32767.0f) x = 32767.0f;
    if (x < -32768.0f) x = -32768.0f;
    out[i] = (int16_t)x;
  }
}

}  // namespace flapcore
