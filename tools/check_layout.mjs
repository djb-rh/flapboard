// Compares web/layout.js with the C++ layout (build/flapsim --layout-grid).
// Usage: node tools/check_layout.mjs   (after tools/flapsim/build.sh)
import { execFileSync } from "node:child_process";
import { createRequire } from "node:module";
const require = createRequire(import.meta.url);
const { computeLayout } = require("../web/layout.js");
const cpp = execFileSync("build/flapsim", ["--layout-grid"], { encoding: "utf8", maxBuffer: 256 * 1024 * 1024 }).trim().split("\n").map(JSON.parse);
let i = 0, bad = 0;
for (let rows = 1; rows <= 12; rows += 1)
  for (let cols = 1; cols <= 40; cols += 3)
    for (const flap_w of [0, 40, 500])
      for (const aspect of [1.0, 1.4, 1.75])
        for (const gap of [0, 4, 10])
          for (let sides = 0; sides < 3; sides++)
            for (const side_pct of [10, 25]) {
              const r = computeLayout({ rows, cols, flap_w, aspect, gap, left_image: sides >= 1, right_image: sides === 2, side_pct });
              const js = { cw: r.cell_w, ch: r.cell_h, bx: r.board.x, by: r.board.y, bw: r.board.w, bh: r.board.h,
                lw: r.left.w, rx: r.right.x, rw: r.right.w, fits: r.fits ? 1 : 0 };
              const c = cpp[i++];
              if (JSON.stringify(js) !== JSON.stringify(c)) {
                if (bad++ < 5) console.log("differs", { rows, cols, flap_w, aspect, gap, sides, side_pct }, "\n  js ", js, "\n  c++", c);
              }
            }
console.log(`${i} layouts compared, ${bad} differ`);
process.exit(bad ? 1 : 0);
