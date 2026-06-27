#include "ss_profile.h"
#include <cstdio>
#include <cstring>

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
    char ident[220];
    if (profile && profile[0]) snprintf(ident, sizeof(ident), "+name \"%s\" +exec \"profiles/%s.cfg\"", profile, profile);
    else                       snprintf(ident, sizeof(ident), "+name P%d", seat + 1);
    // -config gives this seat its OWN ini (created fresh with engine defaults) instead of the user's
    // global zandronum.ini -- so a setting another Zandronum session changed can't bleed into our seats.
    snprintf(buf, cap,
        "-config splitdronum-seat%d.ini -iwad %s -connect 127.0.0.1:%d +set fullscreen 0 +set freelook 1 "
        "+set use_joystick 0 +set use_mouse 0 +exec splitseat.cfg %s"
        "+set vid_defwidth %d +set vid_defheight %d %s",
        seat, iwad, port, audio, pw, ph, ident);
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
    snprintf(buf, cap, "%s%s", ProfileWord1(w1), ProfileWord2(w2));
}

void ProfileBuildCfg(char* buf, int cap, const char* name, int crosshair, int motionComp) {
    int ch = crosshair < 0 ? 0 : (crosshair > kCrosshairMax ? kCrosshairMax : crosshair);
    snprintf(buf, cap,
        "// splitdronum profile -- generated; re-created from the in-game join screen\n"
        "name \"%s\"\n"
        "crosshair %d\n"
        "%s",
        (name && name[0]) ? name : "Player", ch,
        motionComp ? "movebob 0\n" : "");   // motion-sickness compensation = kill the view bob
}

}
