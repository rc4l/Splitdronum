#include "ss_layout.h"
#include <cmath>

namespace ss {

bool operator==(const Rect& a, const Rect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

Layout ComputeLayout(int n, int W, int H, TwoMode mode) {
    Layout out;
    if (n <= 0) return out;            // no active seats
    if (n > 4) n = 4;
    out.count = n;

    if (n == 1) {
        out.panes[0] = Rect{ 0, 0, W, H };
        return out;
    }
    if (n == 2) {
        bool leftRight;
        if (mode == TwoMode::Vertical) {
            leftRight = true;          // vertical divider -> side-by-side
        } else if (mode == TwoMode::Horizontal) {
            leftRight = false;         // horizontal divider -> stacked
        } else {
            double lr = std::fabs((double)(W / 2) / H - 16.0 / 9);   // left/right pane aspect
            double tb = std::fabs((double)W / (H / 2) - 16.0 / 9);   // top/bottom pane aspect
            leftRight = lr <= tb;      // closest-to-16:9 wins
        }
        if (leftRight) {
            out.panes[0] = Rect{ 0, 0, W / 2, H };
            out.panes[1] = Rect{ W / 2, 0, W / 2, H };
        } else {
            out.panes[0] = Rect{ 0, 0, W, H / 2 };
            out.panes[1] = Rect{ 0, H / 2, W, H / 2 };
        }
        return out;
    }
    // 3 or 4 -> quadrant grid (3 leaves the bottom-right empty)
    int hw = W / 2, hh = H / 2;
    out.panes[0] = Rect{ 0, 0, hw, hh };
    out.panes[1] = Rect{ hw, 0, hw, hh };
    out.panes[2] = Rect{ 0, hh, hw, hh };
    if (n == 4) out.panes[3] = Rect{ hw, hh, hw, hh };
    return out;
}

Rect Letterbox(Rect pane, int fw, int fh) {
    double pa = (double)pane.w / pane.h, fa = (double)fw / fh;
    int w, h;
    if (fa > pa) {                                     // content wider -> fit width
        w = pane.w;
        h = (int)std::lround(pane.w / fa);
    } else {                                           // content taller -> fit height
        h = pane.h;
        w = (int)std::lround(pane.h * fa);
    }
    return Rect{ pane.x + (pane.w - w) / 2, pane.y + (pane.h - h) / 2, w, h };
}

void TargetSize(Rect pane, int& rw, int& rh) {
    rw = pane.w;
    rh = pane.h;
    double a = (double)rw / rh;
    if (a > 2.0) {
        rw = (int)(rh * 2.0);          // too wide -> cap width
    } else if (a < 0.6) {
        rh = (int)(rw / 0.6);          // too tall -> cap height
    }
    rw &= ~7;                          // multiple of 8
    rh &= ~7;
    if (rw < 320) rw = 320;
    if (rh < 200) rh = 200;
}

float SmartScale(int liveCount) {
    if (liveCount <= 1) return 1.0f;   // sole seat owns the whole window -> render at full res
    if (liveCount == 2) return 0.75f;  // half the window each -> a touch lower
    return 0.5f;                       // 3-4 seats: a quadrant each -> half res (a quarter the readback)
}

}  // namespace ss
