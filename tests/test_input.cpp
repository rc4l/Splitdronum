#include "../third_party/doctest.h"
#include "../core/ss_input.h"
#include <cstring>

using namespace ss;

// Regression coverage for the stuck-modifier bug: alt-tabbing away left Alt (=+strafe) held, so seat-0
// mouse-X strafed instead of turning. StuckModifierReleases is what the host uses each tick to drop a
// modifier it still holds that the OS reports as physically up.

static void clearHeld(unsigned char* h, int n) { std::memset(h, 0, n); }

TEST_CASE("StuckModifierReleases: nothing held -> nothing released") {
    unsigned char held[0x200]; clearHeld(held, sizeof(held));
    int out[8];
    CHECK(StuckModifierReleases(held, 0x200, false, false, false, out, 8) == 0);
}

TEST_CASE("StuckModifierReleases: held Alt with Alt physically UP -> release it (the strafe bug)") {
    unsigned char held[0x200]; clearHeld(held, sizeof(held));
    held[kScLAlt] = 1;                                  // host thinks Alt is down (=+strafe)
    int out[8];
    int n = StuckModifierReleases(held, 0x200, /*altDown*/false, false, false, out, 8);
    CHECK(n == 1);
    CHECK(out[0] == kScLAlt);
}

TEST_CASE("StuckModifierReleases: held Alt with Alt still DOWN -> keep it (genuine strafe)") {
    unsigned char held[0x200]; clearHeld(held, sizeof(held));
    held[kScLAlt] = 1;
    int out[8];
    CHECK(StuckModifierReleases(held, 0x200, /*altDown*/true, false, false, out, 8) == 0);
}

TEST_CASE("StuckModifierReleases: both Alt scancodes stuck -> both released") {
    unsigned char held[0x200]; clearHeld(held, sizeof(held));
    held[kScLAlt] = 1; held[kScRAlt] = 1;
    int out[8];
    int n = StuckModifierReleases(held, 0x200, false, false, false, out, 8);
    CHECK(n == 2);
    CHECK(out[0] == kScLAlt);
    CHECK(out[1] == kScRAlt);
}

TEST_CASE("StuckModifierReleases: only the physically-up modifiers are released") {
    unsigned char held[0x200]; clearHeld(held, sizeof(held));
    held[kScLCtrl] = 1; held[kScLShift] = 1;           // host holds Ctrl + Shift
    int out[8];
    // Ctrl physically up, Shift still down -> release Ctrl only
    int n = StuckModifierReleases(held, 0x200, false, /*ctrlDown*/false, /*shiftDown*/true, out, 8);
    CHECK(n == 1);
    CHECK(out[0] == kScLCtrl);
}

TEST_CASE("StuckModifierReleases: non-modifier held keys are never released here") {
    unsigned char held[0x200]; clearHeld(held, sizeof(held));
    held[0x11] = 1;                                     // 'W' (DIK) -- a movement key, not a modifier
    int out[8];
    CHECK(StuckModifierReleases(held, 0x200, false, false, false, out, 8) == 0);
}

TEST_CASE("StuckModifierReleases: cap is honored") {
    unsigned char held[0x200]; clearHeld(held, sizeof(held));
    held[kScLAlt] = 1; held[kScRAlt] = 1; held[kScLCtrl] = 1;
    int out[2];
    int n = StuckModifierReleases(held, 0x200, false, false, true, out, 2);   // 3 candidates, cap 2
    CHECK(n == 2);
}

TEST_CASE("StuckModifierReleases: scancodes past the array length are ignored (no OOB)") {
    unsigned char held[0x40]; clearHeld(held, sizeof(held));   // len shorter than the Alt scancodes
    held[kScLCtrl] = 1;                                        // 0x1D < 0x40 -> in range
    int out[8];
    // kScRCtrl (0x9D) and kScLAlt(0x38, in range) etc: only in-range, held ones count
    int n = StuckModifierReleases(held, 0x40, false, false, false, out, 8);
    CHECK(n == 1);
    CHECK(out[0] == kScLCtrl);
}
