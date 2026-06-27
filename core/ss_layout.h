// ss_layout -- pure splitscreen layout math. NO Win32/GL/engine headers: this is the
// kind of logic that gets 100% test coverage. The DLL (per-client hooks) and the host
// (compositor) both link this; the untestable glue stays thin and out of the gate.
#pragma once

namespace ss {

struct Rect { int x = 0, y = 0, w = 0, h = 0; };
bool operator==(const Rect& a, const Rect& b);

// 2-player split orientation. Auto = pick whichever pane shape is closest to 16:9.
enum class TwoMode { Auto, Horizontal, Vertical };

// Up to four pane rects laid out inside the host window.
struct Layout { Rect panes[4]; int count = 0; };

// Pane rects for `n` active seats inside a `W`x`H` window:
//   1 = full; 2 = aspect-aware split (left/right or top/bottom); 3 = three quadrants;
//   4 = quadrants. n<=0 yields an empty layout; n>4 is clamped to 4.
Layout ComputeLayout(int n, int W, int H, TwoMode mode);

// Largest rect of aspect fw:fh centered inside `pane` (letterbox, no distortion).
Rect Letterbox(Rect pane, int fw, int fh);

// Render resolution for a pane: the pane size with extreme aspects capped, rounded
// down to a multiple of 8, clamped to a 320x200 minimum.
void TargetSize(Rect pane, int& rw, int& rh);

// Per-seat render-scale multiplier as a function of the live seat count: more seats means a smaller
// pane each, so render (and read back) proportionally fewer pixels. 1 = 1.0 (full), 2 = 0.75, 3+ = 0.5.
// Multiplied onto the user's base -RenderScale; the host re-applies it live as seats join/leave.
float SmartScale(int liveCount);

}  // namespace ss
