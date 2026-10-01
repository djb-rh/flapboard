// The board geometry, a line-for-line copy of lib/flapcore/src/layout.cpp so
// the web preview shows exactly what the sign will draw.
// tools/check_layout.sh compares the two over a grid of inputs.
(function (root) {
  function clamp(v, lo, hi) { return v < lo ? lo : v > hi ? hi : v; }
  function computeLayout(i) {
    const inp = Object.assign({screen_w: 1280, screen_h: 720, rows: 6, cols: 22, flap_w: 0, aspect: 1.4, gap: 4,
      margin: 12, left_image: false, right_image: false, side_pct: 15, side_gap: 12}, i);
    const r = {fits: true, left: {x: 0, y: 0, w: 0, h: 0}, right: {x: 0, y: 0, w: 0, h: 0}};
    const rows = Math.max(1, inp.rows), cols = Math.max(1, inp.cols);
    const side_w = Math.trunc(inp.screen_w * clamp(inp.side_pct, 0, 40) / 100);
    const left_w = inp.left_image ? side_w : 0, right_w = inp.right_image ? side_w : 0;
    const area_x = inp.margin + (left_w ? left_w + inp.side_gap : 0);
    const area_w = inp.screen_w - area_x - inp.margin - (right_w ? right_w + inp.side_gap : 0);
    const area_y = inp.margin, area_h = inp.screen_h - 2 * inp.margin;
    const aspect = inp.aspect > 0.2 ? inp.aspect : 1.4;
    const by_w = Math.trunc((area_w - (cols - 1) * inp.gap) / cols);
    const by_h = Math.floor(Math.fround((area_h - (rows - 1) * inp.gap) / rows / aspect));
    let cw = Math.max(4, Math.min(by_w, by_h));
    if (inp.flap_w > 0) { if (inp.flap_w <= cw) cw = inp.flap_w; else r.fits = false; }
    const ch = Math.max(4, Math.round(Math.fround(cw * aspect)));
    r.cell_w = cw; r.cell_h = ch; r.pitch_x = cw + inp.gap; r.pitch_y = ch + inp.gap;
    const bw = cols * cw + (cols - 1) * inp.gap, bh = rows * ch + (rows - 1) * inp.gap;
    r.board = {x: area_x + Math.trunc((area_w - bw) / 2), y: area_y + Math.trunc((area_h - bh) / 2), w: bw, h: bh};
    if (left_w) r.left = {x: inp.margin, y: inp.margin, w: r.board.x - inp.side_gap - inp.margin, h: area_h};
    if (right_w) { const x = r.board.x + bw + inp.side_gap; r.right = {x: x, y: inp.margin, w: inp.screen_w - inp.margin - x, h: area_h}; }
    return r;
  }
  root.computeLayout = computeLayout;
  if (typeof module !== "undefined") module.exports = {computeLayout};
})(typeof window !== "undefined" ? window : globalThis);
