#include "../third_party/doctest.h"
#include "../core/ss_join.h"

using namespace ss;

// helper: an input frame with the given newly-pressed buttons (lists sized like the host's defaults)
static JoinInput mk(int btn, int w1c = 16, int w2c = 16, int xc = 11, bool inUse = false, int holdMs = 0) {
    JoinInput in;
    in.newButtons = btn; in.word1Count = w1c; in.word2Count = w2c; in.crosshairCount = xc;
    in.nameInUse = inUse; in.confirmHoldMs = holdMs;
    return in;
}
// a "hold complete" frame (no new buttons; confirm has been held past the threshold)
static JoinInput held(bool inUse = false) { return mk(0, 16, 16, 11, inUse, kJoinHoldMs); }

// XInput bit values (mirror XINPUT_GAMEPAD_*), so the test pins the exact hardware->logic mapping.
enum { XI_DPAD_UP = 0x0001, XI_DPAD_DOWN = 0x0002, XI_DPAD_LEFT = 0x0004, XI_DPAD_RIGHT = 0x0008,
       XI_START = 0x0010, XI_BACK = 0x0020, XI_LB = 0x0100, XI_A = 0x1000, XI_B = 0x2000, XI_X = 0x4000, XI_Y = 0x8000 };

TEST_CASE("PadButtonPressed: edge-detects a fresh press, ignores the unsampled baseline") {
    // not-yet-sampled pad never fires, even if the button reads as held this frame (the replug/startup case)
    CHECK(PadButtonPressed(kPadUnsampled, XI_START, XI_START) == false);
    // clear -> set = fresh press
    CHECK(PadButtonPressed(0, XI_START, XI_START) == true);
    // held across frames = not fresh
    CHECK(PadButtonPressed(XI_START, XI_START, XI_START) == false);
    // released = not a press
    CHECK(PadButtonPressed(XI_START, 0, XI_START) == false);
    // a different button changing doesn't count as START
    CHECK(PadButtonPressed(XI_A, XI_A | XI_START, XI_START) == true);
    CHECK(PadButtonPressed(XI_A, XI_A | XI_B, XI_START) == false);
}

TEST_CASE("JoinButtonsFromXInput: maps dpad + face buttons to JoinButton bits") {
    CHECK(JoinButtonsFromXInput(0) == 0);
    CHECK(JoinButtonsFromXInput(XI_START) == JB_START);
    CHECK(JoinButtonsFromXInput(XI_A) == JB_CONFIRM);   // A/X accept (the variant Yes)
    CHECK(JoinButtonsFromXInput(XI_X) == JB_CONFIRM);
    CHECK(JoinButtonsFromXInput(XI_Y) == JB_BROWSE);    // Y opens the load-existing browser
    CHECK(JoinButtonsFromXInput(XI_B) == JB_CANCEL);
    CHECK(JoinButtonsFromXInput(XI_DPAD_LEFT)  == JB_LEFT);
    CHECK(JoinButtonsFromXInput(XI_DPAD_RIGHT) == JB_RIGHT);
    CHECK(JoinButtonsFromXInput(XI_DPAD_UP)    == JB_UP);
    CHECK(JoinButtonsFromXInput(XI_DPAD_DOWN)  == JB_DOWN);
    // unmapped buttons (BACK, shoulders) produce nothing; combos OR together
    CHECK(JoinButtonsFromXInput(XI_BACK | XI_LB) == 0);
    CHECK(JoinButtonsFromXInput(XI_A | XI_DPAD_DOWN) == (JB_CONFIRM | JB_DOWN));
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

TEST_CASE("JoinAdvance: Left/Right TRAVERSE fields, wrapping") {
    JoinState s; JoinTryStart(s, 1, true, true);
    JoinAdvance(s, mk(JB_LEFT));  CHECK(s.field == JF_MOTION);    // 0 - 1 wraps to last
    JoinAdvance(s, mk(JB_RIGHT)); CHECK(s.field == JF_WORD1);     // last + 1 wraps to 0
    JoinAdvance(s, mk(JB_RIGHT)); CHECK(s.field == JF_WORD2);
    JoinAdvance(s, mk(JB_RIGHT)); CHECK(s.field == JF_CROSSHAIR);
}

TEST_CASE("JoinAdvance: Up/Down CHANGE the focused field's value") {
    JoinState s; JoinTryStart(s, 1, true, true);
    JoinAdvance(s, mk(JB_UP));   CHECK(s.word1 == 1);             // field 0 = word1
    JoinAdvance(s, mk(JB_DOWN)); CHECK(s.word1 == 0);
    JoinAdvance(s, mk(JB_DOWN)); CHECK(s.word1 == 15);           // wrap negative
    s.field = JF_WORD2;     JoinAdvance(s, mk(JB_UP)); CHECK(s.word2 == 1);
    s.field = JF_CROSSHAIR; JoinAdvance(s, mk(JB_UP)); CHECK(s.crosshair == 1);
                            JoinAdvance(s, mk(JB_DOWN)); CHECK(s.crosshair == 0);
    s.field = JF_MOTION;    JoinAdvance(s, mk(JB_UP));   CHECK(s.motion == 1);   // up = on
                            JoinAdvance(s, mk(JB_DOWN)); CHECK(s.motion == 0);   // down = off
    s.field = JF_WORD1; s.word1 = 0; JoinAdvance(s, mk(JB_DOWN, 0)); CHECK(s.word1 == 0);   // empty list -> stays
}

TEST_CASE("JoinHoldPermille: clamps hold progress to 0..1000") {
    CHECK(JoinHoldPermille(0) == 0);
    CHECK(JoinHoldPermille(-50) == 0);
    CHECK(JoinHoldPermille(kJoinHoldMs / 2) == 500);
    CHECK(JoinHoldPermille(kJoinHoldMs) == 1000);
    CHECK(JoinHoldPermille(kJoinHoldMs * 2) == 1000);          // never overshoots
}

TEST_CASE("JoinAdvance: hold-to-confirm creates; a tap/partial hold does NOT; name-in-use blocks; Cancel aborts") {
    JoinState s; JoinTryStart(s, 1, true, true);
    s.word1 = 2; s.word2 = 5; s.crosshair = 3; s.motion = 1;
    // a partial hold (and even a JB_CONFIRM tap) must NOT join -- only a full hold commits
    CHECK(JoinAdvance(s, mk(JB_CONFIRM, 16,16,11,false, kJoinHoldMs - 1)).action == JoinAction::None);
    CHECK(s.pad == 1);                                          // still on the panel
    JoinResult r = JoinAdvance(s, held());                     // hold reached the threshold
    CHECK(r.action == JoinAction::Join);
    CHECK(r.pad == 1); CHECK(r.word1 == 2); CHECK(r.word2 == 5); CHECK(r.crosshair == 3); CHECK(r.motion == 1);
    CHECK(s.pad == -1);                                         // flow ended

    JoinTryStart(s, 1, true, true);
    JoinAdvance(s, mk(JB_CANCEL)); CHECK(s.pad == -1);          // B aborts from the editor
}

TEST_CASE("JoinAdvance: name-in-use opens the variant confirm; Yes joins as variant, No returns to edit") {
    JoinState s; JoinTryStart(s, 1, true, true);
    s.word1 = 2; s.word2 = 5;
    // hold completes on a name that's already loaded -> the variant confirm dialog (no join yet)
    CHECK(JoinAdvance(s, held(/*inUse*/ true)).action == JoinAction::None);
    CHECK(s.mode == JM_VARIANT); CHECK(s.pad == 1);
    // No (Cancel) returns to the editor without joining
    JoinAdvance(s, mk(JB_CANCEL)); CHECK(s.mode == JM_EDIT); CHECK(s.pad == 1);
    // hold again -> variant dialog -> Yes (Confirm) joins, flagged as a variant
    JoinAdvance(s, held(true)); CHECK(s.mode == JM_VARIANT);
    JoinResult r = JoinAdvance(s, mk(JB_CONFIRM));
    CHECK(r.action == JoinAction::Join); CHECK(r.variant == true);
    CHECK(r.word1 == 2); CHECK(r.word2 == 5); CHECK(s.pad == -1); CHECK(s.mode == JM_EDIT);
}

TEST_CASE("JoinAdvance: disconnect aborts the variant dialog too") {
    JoinState s; JoinTryStart(s, 1, true, true);
    JoinAdvance(s, held(/*inUse*/ true)); CHECK(s.mode == JM_VARIANT);
    JoinInput d = mk(0); d.connected = false;
    JoinAdvance(s, d); CHECK(s.pad == -1); CHECK(s.mode == JM_EDIT);
}

// a browse-mode input frame: list of `count` configs, highlighted entry taken or not, optional hold/buttons
static JoinInput brw(int count, bool selTaken = false, int btn = 0, int holdMs = 0) {
    JoinInput in; in.profileCount = count; in.selTaken = selTaken; in.newButtons = btn; in.confirmHoldMs = holdMs;
    return in;
}

TEST_CASE("JoinAdvance: Y opens the browser only when configs exist; Up/Down scroll wraps; B returns") {
    JoinState s; JoinTryStart(s, 1, true, true);
    JoinAdvance(s, mk(JB_BROWSE));            CHECK(s.mode == JM_EDIT);   // no configs -> Y does nothing
    JoinInput open = mk(JB_BROWSE); open.profileCount = 3;
    JoinAdvance(s, open);                     CHECK(s.mode == JM_BROWSE); CHECK(s.browseIndex == 0);
    JoinAdvance(s, brw(3, false, JB_DOWN));   CHECK(s.browseIndex == 1);
    JoinAdvance(s, brw(3, false, JB_UP));     CHECK(s.browseIndex == 0);
    JoinAdvance(s, brw(3, false, JB_UP));     CHECK(s.browseIndex == 2);  // wraps (infinite scroll)
    JoinAdvance(s, brw(3, false, JB_CANCEL)); CHECK(s.mode == JM_EDIT);   // B returns to the editor
}

TEST_CASE("JoinAdvance: browse hold loads a free config; a taken one opens the variant confirm") {
    JoinState s; JoinTryStart(s, 1, true, true);
    JoinInput open = mk(JB_BROWSE); open.profileCount = 4; JoinAdvance(s, open);
    JoinAdvance(s, brw(4, false, JB_DOWN)); JoinAdvance(s, brw(4, false, JB_DOWN));  // browseIndex = 2
    // hold on a FREE config -> join, flagged fromBrowse with the selected index
    JoinResult r = JoinAdvance(s, brw(4, /*selTaken*/ false, 0, kJoinHoldMs));
    CHECK(r.action == JoinAction::Join); CHECK(r.fromBrowse == true); CHECK(r.browseIndex == 2);
    CHECK(r.variant == false); CHECK(s.pad == -1);

    // hold on a TAKEN config -> variant confirm (from browse); Yes joins as a fromBrowse variant
    JoinTryStart(s, 1, true, true); JoinAdvance(s, open);
    JoinAdvance(s, brw(4, true, 0, kJoinHoldMs)); CHECK(s.mode == JM_VARIANT); CHECK(s.variantFrom == JM_BROWSE);
    JoinAdvance(s, mk(JB_CANCEL)); CHECK(s.mode == JM_BROWSE);          // No returns to the browser
    JoinAdvance(s, brw(4, true, 0, kJoinHoldMs)); CHECK(s.mode == JM_VARIANT);
    JoinResult r2 = JoinAdvance(s, mk(JB_CONFIRM));
    CHECK(r2.action == JoinAction::Join); CHECK(r2.variant == true); CHECK(r2.fromBrowse == true);
}
