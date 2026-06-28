// ss_input -- see ss_input.h. Pure logic; unit-tested in tests/test_input.cpp.
#include "ss_input.h"

namespace ss {

int StuckModifierReleases(const unsigned char* held, int len,
                          bool altDown, bool ctrlDown, bool shiftDown,
                          int* out, int cap) {
    // each modifier: the two scancodes that produce it, and whether it's physically down now
    struct Mod { int sc1, sc2; bool down; };
    const Mod mods[3] = {
        { kScLAlt,   kScRAlt,   altDown   },
        { kScLCtrl,  kScRCtrl,  ctrlDown  },
        { kScLShift, kScRShift, shiftDown },
    };
    int n = 0;
    for (const Mod& m : mods) {
        if (m.down) continue;                          // physically held -> not stuck
        const int scs[2] = { m.sc1, m.sc2 };
        for (int sc : scs) {
            if (sc >= 0 && sc < len && held[sc] && n < cap) out[n++] = sc;   // host holds it but key is up
        }
    }
    return n;
}

}  // namespace ss
