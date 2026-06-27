#include "../third_party/doctest.h"
#include "../core/ss_profile.h"
#include <cstdio>
#include <cstring>

using namespace ss;

TEST_CASE("ProfileAddFromFile: strips extension, skips empties / null") {
    ProfileList list;
    CHECK(ProfileAddFromFile(list, "Guest.cfg") == true);
    CHECK(std::strcmp(list.names[0], "Guest") == 0);

    CHECK(ProfileAddFromFile(list, "Player") == true);    // no extension -> kept as-is
    CHECK(std::strcmp(list.names[1], "Player") == 0);

    CHECK(ProfileAddFromFile(list, "a.b.cfg") == true);   // only the last dot is stripped
    CHECK(std::strcmp(list.names[2], "a.b") == 0);

    CHECK(ProfileAddFromFile(list, ".cfg") == false);     // empty after strip
    CHECK(ProfileAddFromFile(list, nullptr) == false);    // no name
    CHECK(list.count == 3);                               // neither skip added
}

TEST_CASE("ProfileAddFromFile: stops at the cap") {
    ProfileList list;
    for (int i = 0; i < kMaxProfiles; ++i) {
        char f[32]; snprintf(f, sizeof(f), "p%d.cfg", i);
        CHECK(ProfileAddFromFile(list, f) == true);
    }
    CHECK(list.count == kMaxProfiles);
    CHECK(ProfileAddFromFile(list, "overflow.cfg") == false);   // full
    CHECK(list.count == kMaxProfiles);
}

TEST_CASE("ProfileFillDefaults: fills only when the list is empty") {
    ProfileList empty;
    ProfileFillDefaults(empty);
    CHECK(empty.count == 3);
    CHECK(std::strcmp(empty.names[0], "Player") == 0);

    ProfileList one;
    ProfileAddFromFile(one, "Solo.cfg");
    ProfileFillDefaults(one);
    CHECK(one.count == 1);                                // untouched
    CHECK(std::strcmp(one.names[0], "Solo") == 0);
}

TEST_CASE("BuildClientArgs: a profile sets +name + +exec, connects to the chosen port, mutes music off seat 0") {
    char buf[820];
    BuildClientArgs(buf, sizeof(buf), 1, "freedoom2.wad", 10670, 1280, 720, "msashbros");
    CHECK(std::strstr(buf, "+name \"msashbros\"") != nullptr);
    CHECK(std::strstr(buf, "+exec \"profiles/msashbros.cfg\"") != nullptr);
    CHECK(std::strstr(buf, "-config splitdronum-seat1.ini") != nullptr);  // own ini, isolated from global
    CHECK(std::strstr(buf, "-connect 127.0.0.1:10670") != nullptr);  // the host's resolved loopback port
    CHECK(std::strstr(buf, "+set snd_musicvolume 0") != nullptr);   // seat 1 mutes music
    CHECK(std::strstr(buf, "-iwad freedoom2.wad") != nullptr);
    CHECK(std::strstr(buf, "vid_defwidth 1280") != nullptr);
}

TEST_CASE("BuildClientArgs: no profile + seat 0 -> default name, music on") {
    char buf[820];
    BuildClientArgs(buf, sizeof(buf), 0, "doom2.wad", 10666, 800, 600, nullptr);
    CHECK(std::strstr(buf, "-config splitdronum-seat0.ini") != nullptr);  // seat 0 own ini
    CHECK(std::strstr(buf, "+name P1") != nullptr);                // seat 0 -> P1
    CHECK(std::strstr(buf, "-connect 127.0.0.1:10666") != nullptr);
    CHECK(std::strstr(buf, "+exec \"profiles/") == nullptr);      // no profile cfg
    CHECK(std::strstr(buf, "snd_musicvolume 0") == nullptr);      // seat 0 keeps music

    char buf2[820];
    BuildClientArgs(buf2, sizeof(buf2), 2, "doom2.wad", 10666, 800, 600, "");   // empty profile == none
    CHECK(std::strstr(buf2, "+name P3") != nullptr);
}

// Regression guard: input died once because every seat shared the user's global zandronum.ini, so a
// setting another Zandronum session changed bled in. Each seat MUST get its own isolated -config.
TEST_CASE("BuildClientArgs: each seat gets its own isolated -config (no shared global ini)") {
    char a[820], b[820], c[820], d[820];
    BuildClientArgs(a, sizeof(a), 0, "iwad.wad", 10666, 960, 540, nullptr);
    BuildClientArgs(b, sizeof(b), 1, "iwad.wad", 10666, 960, 540, "P2");
    BuildClientArgs(c, sizeof(c), 2, "iwad.wad", 10666, 960, 540, "P3");
    BuildClientArgs(d, sizeof(d), 3, "iwad.wad", 10666, 960, 540, "P4");
    CHECK(std::strstr(a, "-config splitdronum-seat0.ini") != nullptr);
    CHECK(std::strstr(b, "-config splitdronum-seat1.ini") != nullptr);
    CHECK(std::strstr(c, "-config splitdronum-seat2.ini") != nullptr);
    CHECK(std::strstr(d, "-config splitdronum-seat3.ini") != nullptr);
    CHECK(std::strstr(a, "splitdronum-seat1") == nullptr);   // seat 0 is NOT seat 1's config
    CHECK(std::strstr(b, "splitdronum-seat0") == nullptr);   // and vice versa -- per-seat, not shared
}

TEST_CASE("BuildClientArgs: render res clamps both bounds and passes through mid-range") {
    char hi[820];
    BuildClientArgs(hi, sizeof(hi), 0, "x.wad", 10666, 5000, 4000, nullptr);    // above the cap
    CHECK(std::strstr(hi, "vid_defwidth 1920") != nullptr);
    CHECK(std::strstr(hi, "vid_defheight 1080") != nullptr);

    char lo[820];
    BuildClientArgs(lo, sizeof(lo), 0, "x.wad", 10666, 100, 50, nullptr);       // below the floor
    CHECK(std::strstr(lo, "vid_defwidth 320") != nullptr);
    CHECK(std::strstr(lo, "vid_defheight 180") != nullptr);

    char mid[820];
    BuildClientArgs(mid, sizeof(mid), 0, "x.wad", 10666, 960, 540, nullptr);    // in range
    CHECK(std::strstr(mid, "vid_defwidth 960") != nullptr);
    CHECK(std::strstr(mid, "vid_defheight 540") != nullptr);
}

TEST_CASE("ProfileComposeName + word lists: two words join, indices wrap") {
    CHECK(ProfileWord1Count() == 16);
    CHECK(ProfileWord2Count() == 16);
    CHECK(std::strcmp(ProfileWord1(0), "angry") == 0);
    CHECK(std::strcmp(ProfileWord2(0), "imp") == 0);
    CHECK(std::strcmp(ProfileWord1(ProfileWord1Count()), "angry") == 0);                 // wraps to 0
    CHECK(std::strcmp(ProfileWord1(-1), ProfileWord1(ProfileWord1Count() - 1)) == 0);    // wraps to last
    char nm[64];
    ProfileComposeName(nm, sizeof(nm), 0, 0);  CHECK(std::strcmp(nm, "angryimp")  == 0);
    ProfileComposeName(nm, sizeof(nm), 1, 1);  CHECK(std::strcmp(nm, "happycaco") == 0);
}

TEST_CASE("ProfileBuildCfg: name + crosshair, movebob only with motion-comp, crosshair clamped") {
    char cfg[256];
    ProfileBuildCfg(cfg, sizeof(cfg), "angryimp", 3, 1);
    CHECK(std::strstr(cfg, "name \"angryimp\"") != nullptr);
    CHECK(std::strstr(cfg, "crosshair 3") != nullptr);
    CHECK(std::strstr(cfg, "movebob 0") != nullptr);            // motion-sickness comp on -> no bob

    ProfileBuildCfg(cfg, sizeof(cfg), "x", 0, 0);
    CHECK(std::strstr(cfg, "crosshair 0") != nullptr);
    CHECK(std::strstr(cfg, "movebob") == nullptr);              // comp off -> engine default bob

    ProfileBuildCfg(cfg, sizeof(cfg), nullptr, -5, 0);          // null name + below-range crosshair
    CHECK(std::strstr(cfg, "name \"Player\"") != nullptr);      // name fallback
    CHECK(std::strstr(cfg, "crosshair 0") != nullptr);          // clamped up to 0

    ProfileBuildCfg(cfg, sizeof(cfg), "y", 99, 0);              // above-range crosshair
    CHECK(std::strstr(cfg, "crosshair 10") != nullptr);         // clamped to kCrosshairMax
}
