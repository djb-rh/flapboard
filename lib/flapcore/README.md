# flapcore

The split-flap sign as a portable library: plain C++17, no Arduino, ESP-IDF or
display library. The host supplies a `Surface` (rectangle blits of RGB565) and,
if it wants sound, listens for flip events.

| Header | What it does |
|---|---|
| `drum.h` | The ordered characters on each flap wheel (Vestaboard-style set with colour tiles) |
| `message.h` | Text -> a grid of drum positions: lines, `{R}`-style colour tiles, wrap, alignment |
| `board.h` | Flap timing: forward-only, fixed time per flap, per-module speed, start staggering, flip events |
| `layout.h` | Screen geometry: cell size (fixed or auto-fit), gaps, optional side images |
| `theme.h` | Colours |
| `glyphs.h` | Pre-rendered top/bottom halves of every drum position at one cell size |
| `truetype.h` | A `FontRaster` over stb_truetype (optional; any 8-bit alpha source works) |
| `render.h` | Draws one cell at any point of a flip into an RGB565 buffer and blits it |

Third-party: `third_party/stb_truetype.h` (v1.26, public domain / MIT).
