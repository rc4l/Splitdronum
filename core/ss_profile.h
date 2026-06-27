// ss_profile -- player profiles + the stock-client launch command they produce. Pure: no Win32 (the
// host does the directory scan + the actual CreateProcess; this module turns filenames into a profile
// list and a chosen profile into a command line), so the testable string/branch logic gets 100% coverage.
#pragma once

namespace ss {

const int kMaxProfiles    = 16;
const int kProfileNameLen = 48;

// A fixed-capacity list of selectable profile display names (POD; the host keeps one global instance).
struct ProfileList {
    char names[kMaxProfiles][kProfileNameLen];
    int  count = 0;
};

// Add a profile from a directory entry filename ("Guest.cfg" -> "Guest"): strips the extension and
// skips empty names / anything past the cap. Returns true if a profile was added.
bool ProfileAddFromFile(ProfileList& list, const char* filename);

// Ensure the list is non-empty: if nothing was found, fill the built-in placeholders.
void ProfileFillDefaults(ProfileList& list);

// Build the full stock-client command line for a seat into buf[cap] (NUL-terminated).
//   uses its own -config splitdronum-seat<seat>.ini (isolated from the global zandronum.ini).
//   connects to 127.0.0.1:<port> (the host's chosen loopback port -- see the host's free-port probe).
//   profile (non-empty) -> '+name "X" +exec "profiles/X.cfg"';  NULL/empty -> '+name P<seat+1>'.
//   seat 0 keeps music; later seats mute it. reqW/reqH are clamped to sane per-client render bounds.
void BuildClientArgs(char* buf, int cap, int seat, const char* iwad, int port, int reqW, int reqH, const char* profile);

// --- in-game profile creator (the controller join screen scrolls these) ---------------------------
// A name is two scrollable words joined: word1 "angry" + word2 "imp" -> "angryimp". Indices wrap.
const int kCrosshairMax = 10;     // crosshair cvar values the screen offers: 0 (off) .. kCrosshairMax

int         ProfileWord1Count();
int         ProfileWord2Count();
const char* ProfileWord1(int i);  // i is wrapped into [0, count)
const char* ProfileWord2(int i);

// Compose "word1word2" into buf[cap] (lowercase, NUL-terminated); indices wrapped into their lists.
void ProfileComposeName(char* buf, int cap, int w1, int w2);

// Build the cfg body for a created profile (written to profiles/<name>.cfg, +exec'd by its seat): sets
// the player name + crosshair, and -- when motion-sickness compensation is on -- movebob 0 (no view bob).
void ProfileBuildCfg(char* buf, int cap, const char* name, int crosshair, int motionComp);

}
