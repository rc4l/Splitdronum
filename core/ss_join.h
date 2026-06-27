// ss_join -- the pure controller drop-in + profile-creator state machine. ONE panel: Start opens it,
// Up/Down move between fields (name word 1, name word 2, crosshair, motion-comfort), Left/Right change the
// focused field, Confirm creates + joins, Cancel aborts. NO Win32/XInput/engine headers: the host maps
// physical buttons to JoinButton bits and owns the side effects (writing the cfg, spawning a client); this
// module only decides the transitions, so it gets 100% test coverage while the glue stays thin.
#pragma once

namespace ss {

// Engine-agnostic join inputs. The host maps XInput START / A,X,Y / B / dpad L,R,U,D onto these bits.
enum JoinButton { JB_START = 1, JB_CONFIRM = 2, JB_CANCEL = 4, JB_LEFT = 8, JB_RIGHT = 16, JB_UP = 32, JB_DOWN = 64 };

enum class JoinAction { None, Join };

// The focusable fields (Up/Down cycle through them).
enum JoinField { JF_WORD1 = 0, JF_WORD2 = 1, JF_CROSSHAIR = 2, JF_MOTION = 3, JF_COUNT = 4 };

// pad < 0 = no flow active. field = the focused field; the rest is the profile being built.
struct JoinState {
    int pad = -1;
    int field = 0;             // JoinField
    int word1 = 0, word2 = 0;  // name parts (indices into the host's word lists)
    int crosshair = 0;
    int motion = 0;            // motion-comfort on/off (1 = movebob killed)
};

struct JoinInput {
    int  newButtons     = 0;     // JoinButton bits NEWLY pressed this frame (for the active controller)
    bool connected      = true;  // is the active controller still connected?
    bool timedOut       = false; // has the flow's idle timeout elapsed?
    int  word1Count     = 1;     // list sizes, for wrapping each scroll
    int  word2Count     = 1;
    int  crosshairCount = 1;
    bool nameInUse      = false; // is the currently-composed name already loaded by a live seat?
};

// On a confirmed create: the chosen fields. The host composes the name, writes profiles/<name>.cfg, spawns.
struct JoinResult {
    JoinAction action = JoinAction::None;
    int pad = -1;
    int word1 = 0, word2 = 0, crosshair = 0, motion = 0;
};

// From idle, a free controller pressing Start (with room) opens the panel (field 0). Returns true if entered.
bool JoinTryStart(JoinState& s, int pad, bool startPressed, bool roomToJoin);

// Advance one frame: Up/Down move the focus, Left/Right change the focused field (motion = on/off), Confirm
// creates (blocked while the composed name is already loaded), Cancel aborts. Mutates s; returns
// {Join, pad, fields} on a confirmed create (ending the flow), else {None}.
JoinResult JoinAdvance(JoinState& s, const JoinInput& in);

}
