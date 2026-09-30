// Flap timing. Every cell turns forward through the drum at one fixed time
// per flap, so a change finishes cell by cell as each reaches its target --
// the stagger is not added, it falls out of the drum order.
//
// Time is milliseconds from any monotonic clock the host likes.
#pragma once

#include <cstdint>
#include <vector>

namespace flapcore {

enum class StartMode { Together, Random, Wave };

struct Motion {
  float flip_ms = 70.0f;           // one flap
  float speed_variance = 0.03f;    // each module is up to +/-3% off (fixed per cell, like real motors)
  StartMode start = StartMode::Random;
  float random_delay_ms = 120.0f;  // StartMode::Random: 0..this per cell
  float wave_ms_per_col = 35.0f;   // StartMode::Wave: left-to-right
  uint32_t seed = 1;
};

struct FlipEvent {
  uint16_t cell;
  uint32_t at_ms;     // when the flap lands (the clack)
};

class FlipSink {
 public:
  virtual ~FlipSink() = default;
  virtual void onFlip(const FlipEvent &e) = 0;
};

struct CellView {
  uint16_t cur = 0;     // what the cell shows (top of the falling flap)
  uint16_t next = 0;    // the position being turned to
  float progress = 0;   // 0..1 through the current flap; meaningful when moving
  bool moving = false;
};

class Board {
 public:
  // Resets every cell to blank (drum position 0).
  void resize(int rows, int cols, int drum_size);
  void setMotion(const Motion &m);
  const Motion &motion() const { return motion_; }

  // Starts turning towards `targets` (rows*cols positions). A cell already
  // mid-flap carries on from where it is, as a real drum would.
  // full_spin: cells already showing their target go round once anyway.
  void show(const std::vector<uint16_t> &targets, uint32_t now_ms, bool full_spin = false);
  // Sets the board instantly (boot, or after the display was off).
  void jump(const std::vector<uint16_t> &targets);

  // Reports every flap that landed in (last update, now]; sink may be null.
  void update(uint32_t now_ms, FlipSink *sink);

  CellView view(int cell, uint32_t now_ms) const;
  bool busy(uint32_t now_ms) const;
  uint32_t finishMs() const { return finish_ms_; }   // when the last cell stops
  // True once per frame for cells that need drawing: while moving, and once
  // more when they stop. Also true for every cell after resize()/jump().
  bool takeDirty(int cell, uint32_t now_ms);

  int rows() const { return rows_; }
  int cols() const { return cols_; }
  int cells() const { return rows_ * cols_; }

 private:
  struct Cell {
    uint16_t from = 0;     // position when the current run started
    uint16_t steps = 0;    // flaps in the current run
    float per = 70;        // ms per flap for this cell
    float speed = 1;       // this module's fixed speed factor
    double start = 0;      // when the first flap of the run began (may be before now)
    uint16_t emitted = 0;  // flaps already reported to the sink
    bool dirty = true;
    bool was_moving = false;
  };
  uint32_t rng();
  int completed(const Cell &c, double now) const;

  std::vector<Cell> cells_;
  Motion motion_;
  int rows_ = 0, cols_ = 0, drum_ = 1;
  uint32_t rng_state_ = 1;
  uint32_t finish_ms_ = 0;
};

}  // namespace flapcore
