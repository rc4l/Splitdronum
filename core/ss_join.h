// ss_join -- the pure controller drop-in + profile-creator state machine. ONE panel: Start opens it,
// Up/Down move between fields (name word 1, name word 2, crosshair, motion-comfort), Left/Right change the
// focused field, Confirm creates + joins, Cancel aborts. NO Win32/XInput/engine headers: the host maps
// physical buttons to JoinButton bits and owns the side effects (writing the cfg, spawning a client); this
// module only decides the transitions, so it gets 100% test coverage while the glue stays thin.
#pragma once

namespace ss {

// Engine-agnostic join inputs. The host maps XInput START / A,X / Y / B / dpad L,R,U,D onto these bits.
enum JoinButton { JB_START = 1, JB_CONFIRM = 2, JB_CANCEL = 4, JB_LEFT = 8, JB_RIGHT = 16, JB_UP = 32, JB_DOWN = 64, JB_BROWSE = 128 };

enum class JoinAction { None, Join };

// The focusable fields (Up/Down cycle through them).
enum JoinField { JF_WORD1 = 0, JF_WORD2 = 1, JF_CROSSHAIR = 2, JF_MOTION = 3, JF_COUNT = 4 };

// Panel mode. EDIT: build the name + settings. BROWSE: scroll the saved-config list to load one. VARIANT:
// the "<name> is in use -- load as <variant>?" Yes/No confirm (reached from EDIT or BROWSE on a taken name).
enum JoinMode { JM_EDIT = 0, JM_VARIANT = 1, JM_BROWSE = 2 };

// pad < 0 = no flow active. field = the focused field; the rest is the profile being built.
struct JoinState {
    int pad = -1;
    int field = 0;             // JoinField
    int word1 = 0, word2 = 0;  // name parts (indices into the host's word lists)
    int crosshair = 0;
    int motion = 0;            // motion-comfort on/off (1 = movebob killed)
    int mode = JM_EDIT;        // JoinMode
    int browseIndex = 0;       // highlighted entry in the saved-config browser
    int variantFrom = JM_EDIT; // which mode opened the variant confirm (so No returns there)
};

struct JoinInput {
    int  newButtons     = 0;     // JoinButton bits NEWLY pressed this frame (for the active controller)
    bool connected      = true;  // is the active controller still connected?
    bool timedOut       = false; // has the flow's idle timeout elapsed?
    int  word1Count     = 1;     // list sizes, for wrapping each scroll
    int  word2Count     = 1;
    int  crosshairCount = 1;
    bool nameInUse      = false; // is the currently-composed name already loaded by a live seat?
    int  confirmHoldMs  = 0;     // how long the confirm input (hold START / Enter) has been held this attempt
    int  profileCount   = 0;     // number of saved configs the browser scrolls (0 = browser unavailable)
    bool selTaken       = false; // is the browser's highlighted config already loaded by a live seat?
};

// How long the confirm input must be held to commit the join. The hold (with the on-screen ring) is what
// prevents an accidental join before settings are finalized -- a tap does nothing.
const int kJoinHoldMs = 700;

// Hold progress as 0..1000 (per-mille) for the overlay ring: clamp(heldMs / kJoinHoldMs). 1000 = ready.
int JoinHoldPermille(int heldMs);

// On a confirmed create: the chosen fields. The host composes the name, writes profiles/<name>.cfg, spawns.
// variant = true when the player accepted loading under a generated variant (the composed name was in use):
// the host substitutes ProfileVariant(composed name) before spawning.
struct JoinResult {
    JoinAction action = JoinAction::None;
    int pad = -1;
    int word1 = 0, word2 = 0, crosshair = 0, motion = 0;
    bool variant = false;       // load under a generated variant (the base name was in use)
    bool fromBrowse = false;    // the base name is the browser's selected config (browseIndex), not the words
    int  browseIndex = 0;       // which saved config was selected, when fromBrowse
};

// --- host glue, extracted as pure logic so the hardware->state-machine bridge is unit-tested too ---

// Sentinel for a controller's previous-button snapshot that hasn't been sampled yet. A pad whose prev is
// this value reports no fresh press, so a button already held when the pad first appears never fires.
const unsigned kPadUnsampled = 0xFFFFu;

// True iff `mask` is freshly pressed this frame: set in cur, clear in prev. prev == kPadUnsampled (not yet
// baselined) returns false. The host uses it to edge-detect the START press that opens the join panel.
bool PadButtonPressed(unsigned prevButtons, unsigned curButtons, unsigned mask);

// Map a raw XInput button bitmask (typically the NEWLY-pressed bits) to ss::JoinButton bits:
// START->JB_START, A/X/Y->JB_CONFIRM, B->JB_CANCEL, dpad->JB_LEFT/RIGHT/UP/DOWN. XInput's XINPUT_GAMEPAD_*
// values are passed as a plain int so core needs no <xinput.h>; the host OR's in stick-derived directions.
int JoinButtonsFromXInput(int xinputButtons);

// From idle, a free controller pressing Start (with room) opens the panel (field 0). Returns true if entered.
bool JoinTryStart(JoinState& s, int pad, bool startPressed, bool roomToJoin);

// Advance one frame: Left/Right TRAVERSE the fields (first name / last name / crosshair / motion), Up/Down
// CHANGE the focused field's value (motion = on/off), and the join COMMITS when confirmHoldMs reaches
// kJoinHoldMs (hold START / Enter -- the ring) provided the composed name isn't already in use. Cancel
// aborts. Mutates s; returns {Join, pad, fields} on commit (ending the flow), else {None}.
JoinResult JoinAdvance(JoinState& s, const JoinInput& in);

}
