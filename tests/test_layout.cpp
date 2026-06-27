#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../third_party/doctest.h"
#include "../core/ss_layout.h"

using namespace ss;

TEST_CASE("ComputeLayout: empty and clamping") {
    CHECK(ComputeLayout(0, 1280, 720, TwoMode::Auto).count == 0);
    CHECK(ComputeLayout(-3, 1280, 720, TwoMode::Auto).count == 0);
    CHECK(ComputeLayout(9, 1280, 720, TwoMode::Auto).count == 4);   // clamped to 4
}

TEST_CASE("ComputeLayout: one player is full screen") {
    auto L = ComputeLayout(1, 1280, 720, TwoMode::Auto);
    CHECK(L.count == 1);
    CHECK(L.panes[0] == Rect{0, 0, 1280, 720});
}

TEST_CASE("ComputeLayout: two-player forced orientations") {
    auto v = ComputeLayout(2, 1280, 720, TwoMode::Vertical);     // side-by-side
    CHECK(v.panes[0] == Rect{0, 0, 640, 720});
    CHECK(v.panes[1] == Rect{640, 0, 640, 720});
    auto h = ComputeLayout(2, 1280, 720, TwoMode::Horizontal);   // stacked
    CHECK(h.panes[0] == Rect{0, 0, 1280, 360});
    CHECK(h.panes[1] == Rect{0, 360, 1280, 360});
}

TEST_CASE("ComputeLayout: two-player auto picks closest to 16:9") {
    auto wide = ComputeLayout(2, 1280, 720, TwoMode::Auto);      // 16:9 -> left/right
    CHECK(wide.panes[0] == Rect{0, 0, 640, 720});
    auto tall = ComputeLayout(2, 720, 1280, TwoMode::Auto);      // portrait -> top/bottom
    CHECK(tall.panes[0] == Rect{0, 0, 720, 640});
    CHECK(tall.panes[1] == Rect{0, 640, 720, 640});
}

TEST_CASE("ComputeLayout: three and four quadrants") {
    auto t = ComputeLayout(3, 1280, 720, TwoMode::Auto);
    CHECK(t.count == 3);
    CHECK(t.panes[0] == Rect{0, 0, 640, 360});
    CHECK(t.panes[1] == Rect{640, 0, 640, 360});
    CHECK(t.panes[2] == Rect{0, 360, 640, 360});
    auto q = ComputeLayout(4, 1280, 720, TwoMode::Auto);
    CHECK(q.count == 4);
    CHECK(q.panes[3] == Rect{640, 360, 640, 360});
}

TEST_CASE("Letterbox: both fit directions, centered") {
    auto wide = Letterbox(Rect{0, 0, 1600, 1000}, 16, 9);   // content wider -> fit width
    CHECK(wide == Rect{0, 50, 1600, 900});
    auto tall = Letterbox(Rect{0, 0, 1000, 1600}, 9, 16);   // content taller -> fit height
    CHECK(tall == Rect{50, 0, 900, 1600});
}

TEST_CASE("TargetSize: passthrough, aspect caps, rounding, minimum") {
    int w, h;
    TargetSize(Rect{0, 0, 1280, 720}, w, h); CHECK(w == 1280); CHECK(h == 720);   // in range
    TargetSize(Rect{0, 0, 2000, 500}, w, h); CHECK(w == 1000); CHECK(h == 496);   // a>2 -> width cap, h rounds 500->496
    TargetSize(Rect{0, 0, 500, 2000}, w, h); CHECK(w == 496);  CHECK(h == 832);   // a<0.6 -> height cap (833->832), w rounds
    TargetSize(Rect{0, 0, 100, 100}, w, h);  CHECK(w == 320);  CHECK(h == 200);   // below minimum -> clamped
}

TEST_CASE("SmartScale: per-seat-count render multiplier") {
    CHECK(SmartScale(0) == 1.0f);    // degenerate (no seats yet) -> full
    CHECK(SmartScale(1) == 1.0f);    // sole seat -> full
    CHECK(SmartScale(2) == 0.75f);   // two up -> a touch lower
    CHECK(SmartScale(3) == 0.5f);    // three/four -> half
    CHECK(SmartScale(4) == 0.5f);
}

TEST_CASE("Rect equality operator") {
    CHECK(Rect{1, 2, 3, 4} == Rect{1, 2, 3, 4});
    CHECK_FALSE(Rect{1, 2, 3, 4} == Rect{0, 2, 3, 4});
}
