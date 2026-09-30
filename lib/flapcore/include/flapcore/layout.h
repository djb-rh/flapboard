// Where everything goes on the screen: the board of cells (fixed flap size
// or the largest that fits) and the optional images either side of it.
#pragma once

namespace flapcore {

struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
};

struct LayoutInput {
  int screen_w = 1280, screen_h = 720;
  int rows = 6, cols = 22;
  int flap_w = 0;          // cell width in px; 0 = largest that fits
  float aspect = 1.4f;     // cell height / width
  int gap = 4;             // between cells
  int margin = 12;         // around everything
  bool left_image = false, right_image = false;
  int side_pct = 15;       // each side image's share of the screen width
  int side_gap = 12;       // between a side image and the board
};

struct LayoutResult {
  Rect board;              // the area the cells occupy
  int cell_w = 0, cell_h = 0;
  int pitch_x = 0, pitch_y = 0;   // cell size + gap
  Rect left, right;        // w == 0 when not shown
  bool fits = true;        // false if a fixed flap_w had to be shrunk
  Rect cell(int r, int c) const { return {board.x + c * pitch_x, board.y + r * pitch_y, cell_w, cell_h}; }
};

LayoutResult computeLayout(const LayoutInput &in);

}  // namespace flapcore
