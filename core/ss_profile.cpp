#include "ss_profile.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace ss {

bool ProfileAddFromFile(ProfileList& list, const char* filename) {
    if (list.count >= kMaxProfiles || !filename) return false;   // full, or no name
    char nm[kProfileNameLen];
    snprintf(nm, sizeof(nm), "%s", filename);
    char* dot = strrchr(nm, '.');
    if (dot) *dot = 0;                                            // strip the extension -> display name
    if (!nm[0]) return false;                                    // ".cfg" / empty -> skip
    snprintf(list.names[list.count], kProfileNameLen, "%s", nm);
    ++list.count;
    return true;
}

void ProfileFillDefaults(ProfileList& list) {
    if (list.count > 0) return;
    const char* def[] = { "Player", "Guest", "Buddy" };
    for (int i = 0; i < 3; ++i) {
        snprintf(list.names[list.count], kProfileNameLen, "%s", def[i]);
        ++list.count;
    }
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void BuildClientArgs(char* buf, int cap, int seat, const char* iwad, int port, int reqW, int reqH, const char* profile) {
    int pw = clampi(reqW, 320, 1920);                            // bound per-client render cost
    int ph = clampi(reqH, 180, 1080);
    const char* audio = (seat == 0) ? "" : "+set snd_musicvolume 0 ";   // only seat 0 plays music
    char ident[220], cfgFile[160];
    if (profile && profile[0]) {
        char disp[kProfileNameLen]; ProfileDisplayName(disp, sizeof(disp), profile);   // "angry_imp_2" -> "angry imp 2"
        snprintf(ident, sizeof(ident), "+name \"%s\" +exec \"profiles/%s.cfg\"", disp, profile);   // spaced name, keyed file
        // -config = the PROFILE's own ini: in-engine changes (keybinds + cvars) save to the profile and
        // reload with it on any seat, instead of sticking to a seat slot. (The +exec cfg above still
        // re-asserts the panel-authoritative name/crosshair/movebob on top each load.)
        snprintf(cfgFile, sizeof(cfgFile), "profiles/%s.ini", profile);
    } else {
        snprintf(ident, sizeof(ident), "+name P%d", seat + 1);
        snprintf(cfgFile, sizeof(cfgFile), "splitdronum-seat%d.ini", seat);   // unnamed seat -> per-slot ini
    }
    // -config gives this client its OWN ini (created fresh with engine defaults) instead of the user's
    // global zandronum.ini -- so a setting another Zandronum session changed can't bleed into our seats.
    // Exec order is the cvar precedence (last wins): the per-config ini, then the PREFERRED baseline
    // (splitseat.cfg), then this seat's profile cfg (in `ident`), then the ABSOLUTE override
    // (splitseat_absolute.cfg) dead-last so it beats profiles + preferred + the host's own +sets above.
    snprintf(buf, cap,
        "-config %s -iwad %s -connect 127.0.0.1:%d +set fullscreen 0 +set freelook 1 "
        "+set use_joystick 0 +set use_mouse 0 +exec splitseat.cfg %s"
        "+set vid_defwidth %d +set vid_defheight %d %s +exec splitseat_absolute.cfg",
        cfgFile, iwad, port, audio, pw, ph, ident);
}

// Doom-flavored word lists -- short + lowercase so word1+word2 reads as one handle ("angryimp").
static const char* const kWord1[] = {
    "angry", "happy", "sneaky", "grim", "rabid", "doomed", "brutal", "swift",
    "cursed", "eager", "mighty", "sleepy", "jolly", "feral", "sly", "bold" };
static const char* const kWord2[] = {
    "imp", "caco", "baron", "demon", "zombie", "revvy", "mando", "vile",
    "pinky", "soul", "cyber", "spider", "knight", "fatso", "arach", "skull" };

static int wrapw(int i, int n) { i %= n; if (i < 0) i += n; return i; }   // lists are never empty

int ProfileWord1Count() { return (int)(sizeof(kWord1) / sizeof(kWord1[0])); }
int ProfileWord2Count() { return (int)(sizeof(kWord2) / sizeof(kWord2[0])); }
const char* ProfileWord1(int i) { return kWord1[wrapw(i, ProfileWord1Count())]; }
const char* ProfileWord2(int i) { return kWord2[wrapw(i, ProfileWord2Count())]; }

void ProfileComposeName(char* buf, int cap, int w1, int w2) {
    snprintf(buf, cap, "%s_%s", ProfileWord1(w1), ProfileWord2(w2));   // "angry" + "baron" -> "angry_baron"
}

static bool nameInList(const char* name, const char* const* taken, int n) {
    for (int i = 0; i < n; ++i) if (taken[i] && strcmp(taken[i], name) == 0) return true;
    return false;
}

void ProfileDisplayName(char* out, int cap, const char* key) {
    int i = 0;
    if (key) for (; key[i] && i < cap - 1; ++i) out[i] = (key[i] == '_') ? ' ' : key[i];
    out[i] = '\0';
}

void ProfileVariant(char* out, int cap, const char* base, const char* const* taken, int n) {
    if (!base) base = "";
    char cand[kProfileNameLen];
    snprintf(cand, sizeof(cand), "%s", base);            // start with the base name
    for (int k = 2; nameInList(cand, taken, n); ++k)     // already taken? bump base_2, base_3, ... until free
        snprintf(cand, sizeof(cand), "%s_%d", base, k);  // (with n taken names, one of base..base_(n+2) must be free)
    snprintf(out, cap, "%s", cand);
}

// The panel is FULLY AUTHORITATIVE: every setting it controls is written UNCONDITIONALLY (a concrete value
// for BOTH states of every field), so loading a profile always re-asserts the exact panel state and nothing
// from in-game or the per-seat ini can bleed through. When you add a panel setting, add an UNCONDITIONAL
// line here -- the "ProfileBuildCfg is fully authoritative" test fails if a managed line is emitted only for
// some field values, and forces you to bump the expected line count when you add one.
void ProfileBuildCfg(char* buf, int cap, const char* name, int crosshair, int motionComp) {
    int ch = crosshair < 0 ? 0 : (crosshair > kCrosshairMax ? kCrosshairMax : crosshair);
    char disp[kProfileNameLen]; ProfileDisplayName(disp, sizeof(disp), (name && name[0]) ? name : "Player");
    snprintf(buf, cap,
        "// splitdronum profile -- generated; re-created from the in-game join screen\n"
        "name \"%s\"\n"                 // authoritative in-game name: the display form (underscores -> spaces)
        "crosshair %d\n"
        "movebob %d\n",
        disp, ch,
        motionComp ? 0 : 1);   // motion-comfort ON = 0 (no view bob); OFF = 1 (engine default), always written
}

bool ProfileParseCfg(const char* cfg, int* crosshair, int* motionComp) {
    if (!cfg) return false;
    int ch = 0, mo = 0; bool found = false;
    for (const char* p = cfg; *p; ) {
        while (*p == ' ' || *p == '\t') ++p;                              // skip any indent
        if (strncmp(p, "crosshair", 9) == 0 && (p[9] == ' ' || p[9] == '\t')) {
            ch = clampi((int)atoi(p + 9), 0, kCrosshairMax); found = true;
        } else if (strncmp(p, "movebob", 7) == 0 && (p[7] == ' ' || p[7] == '\t')) {
            const char* q = p + 7; while (*q == ' ' || *q == '\t') ++q;
            mo = (atoi(q) == 0) ? 1 : 0;                                  // movebob 0 = bob killed = comfort ON
        }
        while (*p && *p != '\n') ++p;                                     // advance to end of line
        if (*p == '\n') ++p;
    }
    if (crosshair) *crosshair = ch;
    if (motionComp) *motionComp = mo;
    return found;
}

}
