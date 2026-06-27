#include "ss_join.h"

namespace ss {

// wrap an index into [0, n). n <= 0 collapses to 0 (empty list).
static int wrap(int v, int n) {
    if (n <= 0) return 0;
    v %= n;
    if (v < 0) v += n;
    return v;
}

bool JoinTryStart(JoinState& s, int pad, bool startPressed, bool roomToJoin) {
    if (s.pad >= 0 || !startPressed || !roomToJoin) return false;   // already flowing / no Start / window full
    s = JoinState();                                               // reset every field
    s.pad = pad;
    return true;
}

JoinResult JoinAdvance(JoinState& s, const JoinInput& in) {
    JoinResult r;
    if (s.pad < 0) return r;                                        // no active flow
    if (!in.connected || in.timedOut) { s.pad = -1; return r; }     // controller gone / idle timeout -> abort
    int nb = in.newButtons;

    if (nb & JB_UP)   s.field = (s.field + JF_COUNT - 1) % JF_COUNT;   // move focus
    if (nb & JB_DOWN) s.field = (s.field + 1) % JF_COUNT;

    int dir = (nb & JB_RIGHT) ? 1 : ((nb & JB_LEFT) ? -1 : 0);      // change the focused field
    if (dir != 0) {
        switch (s.field) {
        case JF_WORD1:     s.word1 = wrap(s.word1 + dir, in.word1Count); break;
        case JF_WORD2:     s.word2 = wrap(s.word2 + dir, in.word2Count); break;
        case JF_CROSSHAIR: s.crosshair = wrap(s.crosshair + dir, in.crosshairCount); break;
        default:           s.motion = (dir > 0) ? 1 : 0; break;     // JF_MOTION: right = on, left = off
        }
    }

    if (nb & JB_CANCEL) s.pad = -1;                                 // B aborts
    else if (nb & JB_CONFIRM) {
        if (!in.nameInUse) {                                        // create + join (host writes cfg, spawns)
            r.action = JoinAction::Join; r.pad = s.pad;
            r.word1 = s.word1; r.word2 = s.word2;
            r.crosshair = s.crosshair; r.motion = s.motion;
            s.pad = -1;                                             // flow ends
        }
        // else: name already loaded by another seat -> stay (host shows "name taken")
    }
    return r;
}

}
