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
    CHECK(s.pad == 2); CHECK(s.step == JS_PROMPT); CHECK(s.word1 == 0);
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

TEST_CASE("JoinAdvance: full create -> Join carries the chosen fields") {
    JoinState s; JoinTryStart(s, 1, true, true);
    JoinAdvance(s, mk(JB_CONFIRM)); CHECK(s.step == JS_WORD1);
    JoinAdvance(s, mk(JB_RIGHT));   CHECK(s.word1 == 1);          // scroll word 1
    JoinAdvance(s, mk(JB_RIGHT));   CHECK(s.word1 == 2);
    JoinAdvance(s, mk(JB_LEFT));    CHECK(s.word1 == 1);
    JoinAdvance(s, mk(JB_CONFIRM)); CHECK(s.step == JS_WORD2);
    JoinAdvance(s, mk(JB_RIGHT));   CHECK(s.word2 == 1);          // scroll word 2
    JoinAdvance(s, mk(JB_CONFIRM)); CHECK(s.step == JS_CROSSHAIR);
    JoinAdvance(s, mk(JB_RIGHT));
    JoinAdvance(s, mk(JB_RIGHT));   CHECK(s.crosshair == 2);      // pick crosshair
    JoinAdvance(s, mk(JB_CONFIRM)); CHECK(s.step == JS_MOTION);
    JoinAdvance(s, mk(JB_LEFT));    CHECK(s.motionComp == 1);     // motion comp on
    JoinResult r = JoinAdvance(s, mk(JB_CONFIRM));
    CHECK(r.action == JoinAction::Join);
    CHECK(r.pad == 1); CHECK(r.word1 == 1); CHECK(r.word2 == 1); CHECK(r.crosshair == 2); CHECK(r.motionComp == 1);
    CHECK(s.pad == -1);                                           // flow ended
}

TEST_CASE("JoinAdvance: Cancel aborts from prompt/word1, backs up a step otherwise") {
    JoinState s; JoinTryStart(s, 1, true, true);
    JoinAdvance(s, mk(JB_CANCEL)); CHECK(s.pad == -1);            // prompt B -> abort
    JoinTryStart(s, 1, true, true);
    JoinAdvance(s, mk(JB_CONFIRM));                               // -> WORD1
    JoinAdvance(s, mk(JB_CANCEL)); CHECK(s.pad == -1);            // word1 B -> abort
    JoinTryStart(s, 1, true, true);
    s.step = JS_WORD2;     JoinAdvance(s, mk(JB_CANCEL)); CHECK(s.step == JS_WORD1);
    s.step = JS_CROSSHAIR; JoinAdvance(s, mk(JB_CANCEL)); CHECK(s.step == JS_WORD2);
    s.step = JS_MOTION;    JoinAdvance(s, mk(JB_CANCEL)); CHECK(s.step == JS_CROSSHAIR);
}

TEST_CASE("JoinAdvance: scroll wraps (negative + empty), motion toggles both ways") {
    JoinState s; JoinTryStart(s, 1, true, true); s.step = JS_WORD1;
    JoinAdvance(s, mk(JB_LEFT)); CHECK(s.word1 == 15);            // 0 - 1 wraps to count-1
    s.word1 = 0; JoinAdvance(s, mk(JB_LEFT, 0)); CHECK(s.word1 == 0);   // empty list -> stays
    s.step = JS_WORD2;     JoinAdvance(s, mk(JB_LEFT)); CHECK(s.word2 == 15);
    s.step = JS_CROSSHAIR; JoinAdvance(s, mk(JB_LEFT)); CHECK(s.crosshair == 10);
    s.step = JS_MOTION;
    JoinAdvance(s, mk(JB_RIGHT)); CHECK(s.motionComp == 1);       // R toggles on
    JoinAdvance(s, mk(JB_RIGHT)); CHECK(s.motionComp == 0);       // R toggles off
}

TEST_CASE("JoinAdvance: create blocked while that name is already loaded") {
    JoinState s; JoinTryStart(s, 1, true, true); s.step = JS_MOTION;
    JoinResult r = JoinAdvance(s, mk(JB_CONFIRM, 16, 16, 11, /*inUse*/ true));
    CHECK(r.action == JoinAction::None);                         // name taken -> no join
    CHECK(s.pad == 1);                                           // still on the screen
}
