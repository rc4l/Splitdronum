#include "ss_join.h"

namespace ss {

// wrap an index into [0, n). n <= 0 collapses to 0 (empty list).
static int wrap(int v, int n) {
    if (n <= 0) return 0;
    v %= n;
    if (v < 0) v += n;
    return v;
}

bool PadButtonPressed(unsigned prevButtons, unsigned curButtons, unsigned mask) {
    if (prevButtons == kPadUnsampled) return false;            // not baselined yet -> never a "fresh" press
    return (curButtons & mask & ~prevButtons) != 0;            // set now, clear before
}

int JoinButtonsFromXInput(int xb) {
    // Mirrors the XINPUT_GAMEPAD_* bit values (kept here as literals so core needs no <xinput.h>).
    enum { XI_DPAD_UP = 0x0001, XI_DPAD_DOWN = 0x0002, XI_DPAD_LEFT = 0x0004, XI_DPAD_RIGHT = 0x0008,
           XI_START = 0x0010, XI_A = 0x1000, XI_B = 0x2000, XI_X = 0x4000, XI_Y = 0x8000 };
    int r = 0;
    if (xb & XI_START)                  r |= JB_START;
    if (xb & (XI_A | XI_X))             r |= JB_CONFIRM;       // A/X accept (the variant Yes)
    if (xb & XI_Y)                      r |= JB_BROWSE;        // Y opens the load-existing browser
    if (xb & XI_B)                      r |= JB_CANCEL;
    if (xb & XI_DPAD_LEFT)              r |= JB_LEFT;
    if (xb & XI_DPAD_RIGHT)             r |= JB_RIGHT;
    if (xb & XI_DPAD_UP)                r |= JB_UP;
    if (xb & XI_DPAD_DOWN)              r |= JB_DOWN;
    return r;
}

bool JoinTryStart(JoinState& s, int pad, bool startPressed, bool roomToJoin) {
    if (s.pad >= 0 || !startPressed || !roomToJoin) return false;   // already flowing / no Start / window full
    s = JoinState();                                               // reset every field
    s.pad = pad;
    return true;
}

int JoinHoldPermille(int heldMs) {
    if (heldMs <= 0) return 0;
    if (heldMs >= kJoinHoldMs) return 1000;
    return heldMs * 1000 / kJoinHoldMs;
}

// Fill r as a Join carrying the current fields, end the flow, and reset the mode.
static void commitJoin(JoinState& s, JoinResult& r, bool variant, bool fromBrowse) {
    r.action = JoinAction::Join; r.pad = s.pad; r.variant = variant;
    r.fromBrowse = fromBrowse; r.browseIndex = s.browseIndex;
    r.word1 = s.word1; r.word2 = s.word2; r.crosshair = s.crosshair; r.motion = s.motion;
    s.pad = -1; s.mode = JM_EDIT;
}

JoinResult JoinAdvance(JoinState& s, const JoinInput& in) {
    JoinResult r;
    if (s.pad < 0) return r;                                        // no active flow
    if (!in.connected || in.timedOut) { s.pad = -1; s.mode = JM_EDIT; return r; }   // gone / idle -> abort
    int nb = in.newButtons;

    if (s.mode == JM_VARIANT) {                                     // "<name> in use -- load as <variant>?" Yes/No
        if (nb & JB_CANCEL)        s.mode = s.variantFrom;         // No -> back where it came from (edit/browse)
        else if (nb & JB_CONFIRM)  commitJoin(s, r, /*variant*/ true, s.variantFrom == JM_BROWSE);   // Yes
        return r;
    }

    if (s.mode == JM_BROWSE) {                                      // scroll the saved configs; hold to load one
        if (nb & JB_CANCEL) { s.mode = JM_EDIT; return r; }        // B -> back to the editor
        if (in.profileCount > 0) {
            if (nb & JB_UP)   s.browseIndex = wrap(s.browseIndex - 1, in.profileCount);   // infinite scroll
            if (nb & JB_DOWN) s.browseIndex = wrap(s.browseIndex + 1, in.profileCount);
            if (in.confirmHoldMs >= kJoinHoldMs) {
                if (in.selTaken) { s.mode = JM_VARIANT; s.variantFrom = JM_BROWSE; }   // taken -> offer a variant
                else             commitJoin(s, r, /*variant*/ false, /*fromBrowse*/ true);   // free -> load it
            }
        }
        return r;
    }

    // JM_EDIT
    if ((nb & JB_BROWSE) && in.profileCount > 0) { s.mode = JM_BROWSE; s.browseIndex = 0; return r; }   // Y -> browser
    if (nb & JB_LEFT)  s.field = (s.field + JF_COUNT - 1) % JF_COUNT;  // Left/Right TRAVERSE the fields
    if (nb & JB_RIGHT) s.field = (s.field + 1) % JF_COUNT;

    int dir = (nb & JB_UP) ? 1 : ((nb & JB_DOWN) ? -1 : 0);        // Up/Down CHANGE the focused field's value
    if (dir != 0) {
        switch (s.field) {
        case JF_WORD1:     s.word1 = wrap(s.word1 + dir, in.word1Count); break;
        case JF_WORD2:     s.word2 = wrap(s.word2 + dir, in.word2Count); break;
        case JF_CROSSHAIR: s.crosshair = wrap(s.crosshair + dir, in.crosshairCount); break;
        default:           s.motion = (dir > 0) ? 1 : 0; break;     // JF_MOTION: up = on, down = off
        }
    }

    if (nb & JB_CANCEL) s.pad = -1;                                 // B aborts
    else if (in.confirmHoldMs >= kJoinHoldMs) {                     // hold-to-confirm completed
        if (!in.nameInUse) commitJoin(s, r, /*variant*/ false, /*fromBrowse*/ false);   // free -> join composed name
        else { s.mode = JM_VARIANT; s.variantFrom = JM_EDIT; }     // already loaded -> ask to load a variant
    }
    return r;
}

}
