// ss_input -- pure seat-input glue logic, shared by the host. NO Qt / Win32 / SDL headers, so it gets
// unit-test coverage like the rest of core/. The host owns the actual event plumbing; this module decides
// the tricky part: which held keys have become "stuck" and must be auto-released.
//
// Why this exists: the host forwards seat-0 key-downs to the engine. If a key-up is lost -- e.g. you
// alt-tab (or cmd-tab on macOS) away and the focus leaves before the key-up arrives -- the engine keeps
// the bind held. With alt = +strafe, a stuck Alt turns all mouse-X into strafing instead of turning (the
// bug this was written for). The host can't trust it always sees the up, so each tick it asks the OS for
// the real modifier state and releases any modifier it still holds that is no longer physically down.
#pragma once

namespace ss {

// DirectInput (DIK) scancodes for the modifier keys the engine binds (left/right Alt, Ctrl, Shift). These
// are the codes the host forwards and the engine maps (e.g. alt = +strafe). Exposed so host + tests agree.
const int kScLAlt = 0x38, kScRAlt = 0xB8;
const int kScLCtrl = 0x1D, kScRCtrl = 0x9D;
const int kScLShift = 0x2A, kScRShift = 0x36;

// Decide which currently-held modifier scancodes are stuck and should be released.
//   held    : array indexed by scancode; held[sc] != 0 means the host currently holds sc down. (len = size)
//   altDown / ctrlDown / shiftDown : is that modifier physically down right now (queried from the OS)?
// For each modifier whose physical key is NOT down, any of its scancodes the host still holds is stuck and
// is written to out[]. Returns the number written (<= cap). out entries are unique scancodes.
int StuckModifierReleases(const unsigned char* held, int len,
                          bool altDown, bool ctrlDown, bool shiftDown,
                          int* out, int cap);

}  // namespace ss
