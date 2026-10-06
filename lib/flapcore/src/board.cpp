#include "flapcore/board.h"

#include <algorithm>
#include <cmath>

namespace flapcore {

uint32_t Board::rng() {   // xorshift32: small, and the same everywhere
  uint32_t x = rng_state_;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return rng_state_ = x ? x : 0x9E3779B9;
}

void Board::resize(int rows, int cols, int drum_size) {
  rows_ = rows;
  cols_ = cols;
  drum_ = drum_size > 0 ? drum_size : 1;
  cells_.assign((size_t)rows * cols, Cell());
  setMotion(motion_);
  finish_ms_ = 0;
}

void Board::setMotion(const Motion &m) {
  motion_ = m;
  // Each module's speed is fixed by its position, not re-rolled per change:
  // on a real board the same slow module is always the last to stop.
  rng_state_ = m.seed ? m.seed : 1;
  for (auto &c : cells_) {
    const float r = (rng() % 20001) / 10000.0f - 1.0f;   // -1..1
    c.speed = 1.0f + r * m.speed_variance;
    c.per = m.flip_ms * c.speed;
  }
}

int Board::completed(const Cell &c, double now) const {
  if (now <= c.start) return 0;
  const int k = (int)std::floor((now - c.start) / c.per);
  return std::min<int>(k, c.steps);
}

void Board::show(const std::vector<uint16_t> &targets, uint32_t now_ms, bool full_spin) {
  if (targets.size() != cells_.size()) return;
  const double now = now_ms;
  finish_ms_ = now_ms;
  for (size_t i = 0; i < cells_.size(); i++) {
    Cell &c = cells_[i];
    c.per = motion_.flip_ms * c.speed;
    const int done = completed(c, now);
    const bool mid_flap = done < c.steps && now > c.start;
    const uint16_t cur = (uint16_t)((c.from + done) % drum_);
    int steps = ((int)targets[i] - (int)cur + drum_) % drum_;
    double start;
    if (mid_flap) {
      // Keep the flap that is falling: the new run begins at that flap.
      const double into = std::fmod(now - c.start, (double)c.per);
      start = now - into;
      if (steps == 0) steps = drum_;   // already past it: all the way round
    } else {
      if (steps == 0 && full_spin) steps = drum_;
      float delay = 0;
      if (motion_.start == StartMode::Random) delay = (rng() % 10001) / 10000.0f * motion_.random_delay_ms;
      else if (motion_.start == StartMode::Wave) delay = (float)(i % cols_) * motion_.wave_ms_per_col;
      start = now + delay;
    }
    // Landings already reported ahead of time (update's lookahead) that the
    // new run shares: a run that keeps the falling flap has the same timing,
    // so its first landings are those same ones and must not be reported twice.
    const int ahead = mid_flap ? std::max(0, (int)c.emitted - done) : 0;
    c.from = cur;
    c.steps = (uint16_t)steps;
    c.start = start;
    c.emitted = (uint16_t)std::min(ahead, steps);
    if (steps) c.dirty = true;
    const double end = start + steps * (double)c.per;
    if (end > finish_ms_) finish_ms_ = (uint32_t)std::ceil(end);
  }
}

void Board::jump(const std::vector<uint16_t> &targets) {
  if (targets.size() != cells_.size()) return;
  for (size_t i = 0; i < cells_.size(); i++) {
    Cell &c = cells_[i];
    c.from = targets[i];
    c.steps = 0;
    c.emitted = 0;
    c.dirty = true;
    c.was_moving = false;
  }
}

void Board::update(uint32_t now_ms, FlipSink *sink, uint32_t lookahead_ms) {
  const double now = (double)now_ms + lookahead_ms;
  for (size_t i = 0; i < cells_.size(); i++) {
    Cell &c = cells_[i];
    const int done = completed(c, now);
    while (c.emitted < done) {
      c.emitted++;
      if (sink) sink->onFlip({(uint16_t)i, (uint32_t)std::lround(c.start + c.emitted * (double)c.per)});
    }
  }
}

CellView Board::view(int i, uint32_t now_ms) const {
  const Cell &c = cells_[i];
  const double now = now_ms;
  const int done = completed(c, now);
  CellView v;
  v.cur = (uint16_t)((c.from + done) % drum_);
  v.next = (uint16_t)((v.cur + 1) % drum_);
  v.moving = done < c.steps && now > c.start;
  if (v.moving) v.progress = (float)((now - c.start - done * (double)c.per) / c.per);
  return v;
}

bool Board::busy(uint32_t now_ms) const { return now_ms < finish_ms_; }

void Board::markAllDirty() {
  for (auto &c : cells_) {
    c.dirty = true;
    c.drawn = 0xFFFF;
  }
}

bool Board::takeDirty(int i, uint32_t now_ms) {
  Cell &c = cells_[i];
  const CellView v = view(i, now_ms);
  // was_moving: draw the landed state once; drawn: the resting face on the
  // screen isn't the one the cell is at (its whole run fell between frames)
  const bool d = c.dirty || v.moving || c.was_moving || (!v.moving && v.cur != c.drawn);
  c.was_moving = v.moving;
  c.dirty = false;
  if (d) c.drawn = v.moving ? 0xFFFF : v.cur;
  return d;
}

}  // namespace flapcore
