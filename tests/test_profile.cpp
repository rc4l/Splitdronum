#include "../third_party/doctest.h"
#include "../core/ss_profile.h"
#include <cstdio>
#include <cstring>

using namespace ss;

TEST_CASE("ProfileVariant: base when free, else first free numbered variant") {
    char out[48];
    ProfileVariant(out, sizeof(out), "angry_baron", nullptr, 0);
    CHECK(std::strcmp(out, "angry_baron") == 0);                // nothing taken -> base itself

    const char* taken1[] = { "angry_baron" };
    ProfileVariant(out, sizeof(out), "angry_baron", taken1, 1);
    CHECK(std::strcmp(out, "angry_baron_2") == 0);              // base taken -> base_2

    const char* taken2[] = { "angry_baron", "angry_baron_2", "angry_baron_3" };
    ProfileVariant(out, sizeof(out), "angry_baron", taken2, 3);
    CHECK(std::strcmp(out, "angry_baron_4") == 0);             // skips taken variants

    const char* taken3[] = { "angry_baron", "angry_baron_3" }; // gap reused: _2 is free
    ProfileVariant(out, sizeof(out), "angry_baron", taken3, 2);
    CHECK(std::strcmp(out, "angry_baron_2") == 0);

    const char* other[] = { "happy_imp", "grim_caco" };        // unrelated names don't block the base
    ProfileVariant(out, sizeof(out), "angry_baron", other, 2);
    CHECK(std::strcmp(out, "angry_baron") == 0);
}

TEST_CASE("ProfileDisplayName: underscores -> spaces; spaced name is the authoritative in-game +name") {
    char d[48];
    ProfileDisplayName(d, sizeof(d), "angry_imp");    CHECK(std::strcmp(d, "angry imp") == 0);
    ProfileDisplayName(d, sizeof(d), "angry_imp_2");  CHECK(std::strcmp(d, "angry imp 2") == 0);
    ProfileDisplayName(d, sizeof(d), "nounderscore"); CHECK(std::strcmp(d, "nounderscore") == 0);
    ProfileDisplayName(d, sizeof(d), "");             CHECK(std::strcmp(d, "") == 0);
    ProfileDisplayName(d, sizeof(d), nullptr);        CHECK(std::strcmp(d, "") == 0);

    // the launch command uses the SPACED name for +name, but the underscore KEY for the cfg file path
    char buf[1024];
    BuildClientArgs(buf, sizeof(buf), 1, "freedoom2.wad", 10666, 1280, 720, "angry_imp_2");
    CHECK(std::strstr(buf, "+name \"angry imp 2\"") != nullptr);
    CHECK(std::strstr(buf, "profiles/angry_imp_2.cfg") != nullptr);

    // and the cfg's name cvar is the spaced display too (so it's authoritative in-game)
    char cfg[256];
    ProfileBuildCfg(cfg, sizeof(cfg), "angry_imp_2", 3, 0);
    CHECK(std::strstr(cfg, "name \"angry imp 2\"") != nullptr);
}

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
    CHECK(std::strstr(buf, "-config profiles/msashbros.ini") != nullptr); // per-PROFILE ini (follows the player)
    CHECK(std::strstr(buf, "-connect 127.0.0.1:10670") != nullptr);  // the host's resolved loopback port
    CHECK(std::strstr(buf, "+set snd_musicvolume 0") != nullptr);   // seat 1 mutes music
    CHECK(std::strstr(buf, "-iwad freedoom2.wad") != nullptr);
    CHECK(std::strstr(buf, "vid_defwidth 1280") != nullptr);
}

TEST_CASE("BuildClientArgs: config exec order = precedence (preferred < profile cfg < absolute, last)") {
    char buf[820];
    BuildClientArgs(buf, sizeof(buf), 1, "freedoom2.wad", 10666, 1280, 720, "angry_baron");
    const char* preferred = std::strstr(buf, "+exec splitseat.cfg");
    const char* profile   = std::strstr(buf, "+exec \"profiles/angry_baron.cfg\"");
    const char* absolute  = std::strstr(buf, "+exec splitseat_absolute.cfg");
    CHECK(preferred != nullptr); CHECK(profile != nullptr); CHECK(absolute != nullptr);
    CHECK(preferred < profile);    // baseline runs before the profile cfg (profile overrides it)
    CHECK(profile < absolute);     // absolute runs AFTER the profile cfg -> it wins (final say)

    // it's emitted even with no profile (so the override always applies)
    char np[820];
    BuildClientArgs(np, sizeof(np), 0, "doom2.wad", 10666, 800, 600, nullptr);
    CHECK(std::strstr(np, "+exec splitseat_absolute.cfg") != nullptr);
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
// setting another Zandronum session changed bled in. Each client MUST get its own isolated -config -- a
// per-PROFILE ini when it has one (so in-engine changes follow the player, not the seat slot), else a
// per-slot ini for an unnamed seat.
TEST_CASE("BuildClientArgs: profiled seats get a per-profile -config; unnamed seats a per-slot one") {
    char a[820], b[820], c[820];
    BuildClientArgs(a, sizeof(a), 0, "iwad.wad", 10666, 960, 540, nullptr);        // unnamed -> per-slot
    BuildClientArgs(b, sizeof(b), 1, "iwad.wad", 10666, 960, 540, "angry_baron");  // profiled -> per-profile
    BuildClientArgs(c, sizeof(c), 2, "iwad.wad", 10666, 960, 540, "happy_imp");
    CHECK(std::strstr(a, "-config splitdronum-seat0.ini") != nullptr);
    CHECK(std::strstr(b, "-config profiles/angry_baron.ini") != nullptr);          // follows the profile
    CHECK(std::strstr(c, "-config profiles/happy_imp.ini") != nullptr);
    CHECK(std::strstr(b, "splitdronum-seat") == nullptr);   // a profiled seat is NOT keyed to its slot
    CHECK(std::strstr(b, "profiles/happy_imp") == nullptr);  // and each profile's ini is its own
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
    ProfileComposeName(nm, sizeof(nm), 0, 0);  CHECK(std::strcmp(nm, "angry_imp")  == 0);
    ProfileComposeName(nm, sizeof(nm), 1, 1);  CHECK(std::strcmp(nm, "happy_caco") == 0);
}

TEST_CASE("ProfileBuildCfg: name + crosshair + movebob (always written), crosshair clamped") {
    char cfg[256];
    ProfileBuildCfg(cfg, sizeof(cfg), "angryimp", 3, 1);
    CHECK(std::strstr(cfg, "name \"angryimp\"") != nullptr);
    CHECK(std::strstr(cfg, "crosshair 3") != nullptr);
    CHECK(std::strstr(cfg, "movebob 0") != nullptr);            // motion-comfort on -> no bob

    ProfileBuildCfg(cfg, sizeof(cfg), "x", 0, 0);
    CHECK(std::strstr(cfg, "crosshair 0") != nullptr);
    CHECK(std::strstr(cfg, "movebob 1") != nullptr);            // comfort off -> bob written back to default (1)

    ProfileBuildCfg(cfg, sizeof(cfg), nullptr, -5, 0);          // null name + below-range crosshair
    CHECK(std::strstr(cfg, "name \"Player\"") != nullptr);      // name fallback
    CHECK(std::strstr(cfg, "crosshair 0") != nullptr);          // clamped up to 0

    ProfileBuildCfg(cfg, sizeof(cfg), "y", 99, 0);              // above-range crosshair
    CHECK(std::strstr(cfg, "crosshair 10") != nullptr);         // clamped to kCrosshairMax
}

// Guard: the panel must be FULLY AUTHORITATIVE -- every setting it manages is written for EVERY field value
// (never conditionally), so a reload always re-asserts the panel and no in-game / per-seat-ini value bleeds
// through. This counts the non-comment cvar lines and requires it to be (a) identical across all field values
// and (b) exactly kManagedSettings. Add a panel setting -> write it unconditionally AND bump the count here;
// emit a line only for some values and the "identical across values" check fails. Either way you can't forget.
static int cvarLines(const char* cfg) {
    int n = 0;
    for (const char* p = cfg; *p; ) {
        while (*p == ' ' || *p == '\t') ++p;
        if (*p != '\n' && *p != '\0' && !(p[0] == '/' && p[1] == '/')) ++n;   // not blank, not a comment
        while (*p && *p != '\n') ++p;
        if (*p == '\n') ++p;
    }
    return n;
}
TEST_CASE("ProfileBuildCfg is fully authoritative: same cvars written for every field value") {
    const int kManagedSettings = 3;     // name, crosshair, movebob -- bump when you add a panel setting
    char a[256], b[256], c[256], d[256];
    ProfileBuildCfg(a, sizeof(a), "p", 0, 0);   // comfort off, crosshair min
    ProfileBuildCfg(b, sizeof(b), "p", 5, 1);   // comfort on,  crosshair mid
    ProfileBuildCfg(c, sizeof(c), "p", kCrosshairMax, 0);
    ProfileBuildCfg(d, sizeof(d), "p", 2, 1);
    CHECK(cvarLines(a) == kManagedSettings);
    CHECK(cvarLines(a) == cvarLines(b));        // no setting appears/disappears with a field's value
    CHECK(cvarLines(a) == cvarLines(c));
    CHECK(cvarLines(a) == cvarLines(d));
}

TEST_CASE("ProfileParseCfg: reads crosshair + motion back, round-trips, tolerant of junk") {
    int ch = -1, mo = -1;
    CHECK(ProfileParseCfg("name \"angryimp\"\ncrosshair 3\nmovebob 0\n", &ch, &mo) == true);
    CHECK(ch == 3); CHECK(mo == 1);                            // movebob 0 -> motion comfort on

    ch = -1; mo = -1;
    CHECK(ProfileParseCfg("crosshair 7\n", &ch, &mo) == true); // no movebob line -> comfort off
    CHECK(ch == 7); CHECK(mo == 0);

    ch = -1; mo = -1;
    CHECK(ProfileParseCfg("crosshair 1\nmovebob 1\n", &ch, &mo) == true);
    CHECK(mo == 0);                                            // movebob non-zero -> bob kept

    ch = -1;
    CHECK(ProfileParseCfg("  crosshair 99\r\n", &ch, nullptr) == true);   // indent + CRLF, null out-param
    CHECK(ch == kCrosshairMax);                               // clamped

    CHECK(ProfileParseCfg("name \"x\"\n", nullptr, nullptr) == false);    // no crosshair -> not a profile
    CHECK(ProfileParseCfg("", &ch, &mo) == false);
    CHECK(ProfileParseCfg(nullptr, &ch, &mo) == false);

    char cfg[256]; int rch = 0, rmo = 0;                       // round-trip through ProfileBuildCfg
    ProfileBuildCfg(cfg, sizeof(cfg), "sneakyrevvy", 5, 1);
    CHECK(ProfileParseCfg(cfg, &rch, &rmo) == true);
    CHECK(rch == 5); CHECK(rmo == 1);
}
