// ss_join -- the pure controller drop-in + profile-creator state machine. Flow: Start -> prompt ->
// scroll word 1 -> scroll word 2 -> pick crosshair -> toggle motion-sickness comp -> create + join.
// NO Win32/XInput/engine headers: the host maps physical buttons to JoinButton bits and owns the side
// effects (writing the cfg, spawning a client); this module only decides the transitions, so it gets
// 100% test coverage while the XInput/thread/D3D glue around it stays thin and out of the gate.
#pragma once

namespace ss {

// Engine-agnostic join inputs. The host maps XInput START / A,X,Y / B / dpad L,R onto these bits.
enum JoinButton { JB_START = 1, JB_CONFIRM = 2, JB_CANCEL = 4, JB_LEFT = 8, JB_RIGHT = 16 };

enum class JoinAction { None, Join };

// Steps of the create-and-join flow.
enum JoinStep { JS_PROMPT = 0, JS_WORD1 = 1, JS_WORD2 = 2, JS_CROSSHAIR = 3, JS_MOTION = 4 };

// pad < 0 = no flow active. The other fields are the profile being built, scrolled step by step.
struct JoinState {
    int pad = -1;
    int step = JS_PROMPT;
    int word1 = 0, word2 = 0;   // name parts (indices into the host's word lists)
    int crosshair = 0;          // crosshair cvar value
    int motionComp = 0;         // motion-sickness compensation on/off (1 = movebob killed)
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

// Result on a confirmed create: the chosen fields. The host composes the name, writes profiles/<name>.cfg
// and spawns the seat. action stays None until the final confirm.
struct JoinResult {
    JoinAction action = JoinAction::None;
    int pad = -1;
    int word1 = 0, word2 = 0, crosshair = 0, motionComp = 0;
};

// From an idle state, a free controller pressing Start (with room) opens the prompt. Returns true if entered.
bool JoinTryStart(JoinState& s, int pad, bool startPressed, bool roomToJoin);

// Advance an active flow by one frame: L/R scroll the current field, Confirm advances (and creates on the
// last step), Cancel backs out a step (or aborts from the prompt / first field). A create is blocked when
// the composed name is already loaded (two seats can't load the same profile). Mutates s; returns
// {Join, pad, fields} on a confirmed create (ending the flow), else {None}.
JoinResult JoinAdvance(JoinState& s, const JoinInput& in);

}
