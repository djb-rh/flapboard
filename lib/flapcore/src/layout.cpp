#include "flapcore/layout.h"

#include <algorithm>
#include <cmath>

namespace flapcore {

LayoutResult computeLayout(const LayoutInput &in) {
  LayoutResult r;
  const int rows = std::max(1, in.rows), cols = std::max(1, in.cols);
  const int side_w = in.screen_w * std::clamp(in.side_pct, 0, 40) / 100;
  int left_w = in.left_image ? side_w : 0, right_w = in.right_image ? side_w : 0;
  const int area_x = in.margin + (left_w ? left_w + in.side_gap : 0);
  const int area_w = in.screen_w - area_x - in.margin - (right_w ? right_w + in.side_gap : 0);
  const int area_y = in.margin, area_h = in.screen_h - 2 * in.margin;
  const float aspect = in.aspect > 0.2f ? in.aspect : 1.4f;

  // The largest cell width whose board fits both ways.
  const int by_w = (area_w - (cols - 1) * in.gap) / cols;
  const int by_h = (int)std::floor((area_h - (rows - 1) * in.gap) / (float)rows / aspect);
  int cw = std::max(4, std::min(by_w, by_h));
  if (in.flap_w > 0) {
    if (in.flap_w <= cw) cw = in.flap_w;
    else r.fits = false;
  }
  const int ch = std::max(4, (int)std::lround(cw * aspect));
  r.cell_w = cw;
  r.cell_h = ch;
  r.pitch_x = cw + in.gap;
  r.pitch_y = ch + in.gap;
  const int bw = cols * cw + (cols - 1) * in.gap, bh = rows * ch + (rows - 1) * in.gap;
  r.board = {area_x + (area_w - bw) / 2, area_y + (area_h - bh) / 2, bw, bh};
  // Side images take the space between the screen edge and the board.
  if (left_w) r.left = {in.margin, in.margin, r.board.x - in.side_gap - in.margin, area_h};
  if (right_w) {
    const int x = r.board.x + bw + in.side_gap;
    r.right = {x, in.margin, in.screen_w - in.margin - x, area_h};
  }
  return r;
}

}  // namespace flapcore
