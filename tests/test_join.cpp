#include "../third_party/doctest.h"
#include "../core/ss_join.h"

using namespace ss;

// helper: an input frame with the given newly-pressed buttons (lists sized like the host's defaults)
static JoinInput mk(int btn, int w1c = 16, int w2c = 16, int xc = 11, bool inUse = false) {
    JoinInput in;
    in.newButtons = btn; in.word1Count = w1c; in.word2Count = w2c; in.crosshairCount = xc; in.nameInUse = inUse;
    return in;
}

TEST_CASE("JoinTryStart: opens only when free + Start + room") {
    JoinState s;
    CHECK(JoinTryStart(s, 2, true, true) == true);
    CHECK(s.pad == 2); CHECK(s.field == 0); CHECK(s.word1 == 0);
    CHECK(JoinTryStart(s, 3, true, true) == false);     // already flowing
    JoinState a, b, c;
    CHECK(JoinTryStart(a, 0, false, true)  == false);   // no Start
    CHECK(JoinTryStart(b, 0, true,  false) == false);   // no room
    CHECK(JoinTryStart(c, 0, true,  true)  == true);
}

TEST_CASE("JoinAdvance: idle + abort paths") {
    JoinState s;                                         // pad = -1 (idle)
    CHECK(JoinAdvance(s, mk(JB_CONFIRM)).action == JoinAction::None);
    JoinTryStart(s, 1, true, true);
    JoinInput d = mk(0); d.connected = false;
    JoinAdvance(s, d); CHECK(s.pad == -1);              // disconnect aborts
    JoinTryStart(s, 1, true, true);
    JoinInput t = mk(0); t.timedOut = true;
    JoinAdvance(s, t); CHECK(s.pad == -1);              // idle timeout aborts
}

TEST_CASE("JoinAdvance: Up/Down move focus, wrapping") {
    JoinState s; JoinTryStart(s, 1, true, true);
    JoinAdvance(s, mk(JB_UP));   CHECK(s.field == JF_MOTION);     // 0 - 1 wraps to last
    JoinAdvance(s, mk(JB_DOWN)); CHECK(s.field == JF_WORD1);      // last + 1 wraps to 0
    JoinAdvance(s, mk(JB_DOWN)); CHECK(s.field == JF_WORD2);
    JoinAdvance(s, mk(JB_DOWN)); CHECK(s.field == JF_CROSSHAIR);
}

TEST_CASE("JoinAdvance: Left/Right change the focused field") {
    JoinState s; JoinTryStart(s, 1, true, true);
    JoinAdvance(s, mk(JB_RIGHT)); CHECK(s.word1 == 1);            // field 0 = word1
    JoinAdvance(s, mk(JB_LEFT));  CHECK(s.word1 == 0);
    JoinAdvance(s, mk(JB_LEFT));  CHECK(s.word1 == 15);          // wrap negative
    s.field = JF_WORD2;     JoinAdvance(s, mk(JB_RIGHT)); CHECK(s.word2 == 1);
    s.field = JF_CROSSHAIR; JoinAdvance(s, mk(JB_RIGHT)); CHECK(s.crosshair == 1);
                            JoinAdvance(s, mk(JB_LEFT));  CHECK(s.crosshair == 0);
    s.field = JF_MOTION;    JoinAdvance(s, mk(JB_RIGHT)); CHECK(s.motion == 1);   // right = on
                            JoinAdvance(s, mk(JB_LEFT));  CHECK(s.motion == 0);   // left = off
    s.field = JF_WORD1; s.word1 = 0; JoinAdvance(s, mk(JB_LEFT, 0)); CHECK(s.word1 == 0);   // empty list -> stays
}

TEST_CASE("JoinAdvance: Confirm creates (carrying fields); name-in-use blocks; Cancel aborts") {
    JoinState s; JoinTryStart(s, 1, true, true);
    s.word1 = 2; s.word2 = 5; s.crosshair = 3; s.motion = 1;
    JoinResult r = JoinAdvance(s, mk(JB_CONFIRM));
    CHECK(r.action == JoinAction::Join);
    CHECK(r.pad == 1); CHECK(r.word1 == 2); CHECK(r.word2 == 5); CHECK(r.crosshair == 3); CHECK(r.motion == 1);
    CHECK(s.pad == -1);                                          // flow ended

    JoinTryStart(s, 1, true, true);
    JoinResult r2 = JoinAdvance(s, mk(JB_CONFIRM, 16, 16, 11, /*inUse*/ true));
    CHECK(r2.action == JoinAction::None);                       // name taken -> no join
    CHECK(s.pad == 1);                                          // still on the panel
    JoinAdvance(s, mk(JB_CANCEL)); CHECK(s.pad == -1);          // B aborts
}
