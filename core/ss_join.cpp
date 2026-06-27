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
    switch (s.step) {
    case JS_PROMPT:                                                 // "press A to join"
        if (nb & JB_CANCEL)       s.pad = -1;                       // B cancels
        else if (nb & JB_CONFIRM) s.step = JS_WORD1;               // A/X/Y -> start building the name
        break;
    case JS_WORD1:                                                  // scroll the first name word
        if (nb & JB_LEFT)  s.word1 = wrap(s.word1 - 1, in.word1Count);
        if (nb & JB_RIGHT) s.word1 = wrap(s.word1 + 1, in.word1Count);
        if (nb & JB_CANCEL)       s.pad = -1;                       // B from the first field aborts the join
        else if (nb & JB_CONFIRM) s.step = JS_WORD2;
        break;
    case JS_WORD2:                                                  // scroll the second name word
        if (nb & JB_LEFT)  s.word2 = wrap(s.word2 - 1, in.word2Count);
        if (nb & JB_RIGHT) s.word2 = wrap(s.word2 + 1, in.word2Count);
        if (nb & JB_CANCEL)       s.step = JS_WORD1;                // B backs up a step
        else if (nb & JB_CONFIRM) s.step = JS_CROSSHAIR;
        break;
    case JS_CROSSHAIR:                                              // pick a crosshair
        if (nb & JB_LEFT)  s.crosshair = wrap(s.crosshair - 1, in.crosshairCount);
        if (nb & JB_RIGHT) s.crosshair = wrap(s.crosshair + 1, in.crosshairCount);
        if (nb & JB_CANCEL)       s.step = JS_WORD2;
        else if (nb & JB_CONFIRM) s.step = JS_MOTION;
        break;
    case JS_MOTION:                                                 // motion-sickness compensation on/off
        if (nb & (JB_LEFT | JB_RIGHT)) s.motionComp = !s.motionComp;
        if (nb & JB_CANCEL)       s.step = JS_CROSSHAIR;
        else if (nb & JB_CONFIRM) {
            if (!in.nameInUse) {                                    // create + join (host writes cfg, spawns)
                r.action = JoinAction::Join; r.pad = s.pad;
                r.word1 = s.word1; r.word2 = s.word2;
                r.crosshair = s.crosshair; r.motionComp = s.motionComp;
                s.pad = -1;                                         // flow ends
            }
            // else: name already loaded by another seat -> stay here (host shows "name taken")
        }
        break;
    }
    return r;
}

}
