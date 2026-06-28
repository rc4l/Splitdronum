// host_sdl -- the cross-platform splitscreen compositor (Windows + macOS).
//
// This is the multiplatform sibling of host/host.cpp (the legacy Win32/D3D11/Qt host). It does the
// same job -- launch N stock Zandronum clients, read each client's framebuffer from shared memory, and
// composite them into ONE window -- but on a portable stack:
//
//   window + GPU compositor : SDL2 + SDL_Renderer (D3D11 on Windows, Metal on macOS, automatically)
//   gamepads                : SDL_GameController
//   look / aim mouse        : SDL relative mouse
//   shared memory           : ss_shm.h shim (Win32 file-mapping / POSIX shm_open+mmap)
//   client launch + inject  : ss_proc.h shim (Win32 CreateProcess+CreateRemoteThread / POSIX posix_spawn
//                             + DYLD_INSERT_LIBRARIES)
//
// All the decision logic (layout, the controller join state machine, profiles) is the pure, fully
// tested core/ library -- shared verbatim with the Win32 host. Only the platform glue differs.
//
// Build: enabled by -DSS_BUILD_HOST=ON (see CMakeLists.txt). The CI matrix compiles + links it on both
// Windows and macOS; runtime parity on macOS additionally needs the engine-side capture hook ported.
//
//   host <numPlayers> <hook-lib> <zandronum> <iwad> [gamedir] [seats_preferred.cfg] [seats_absolute.cfg]
#define SDL_MAIN_HANDLED            // we provide a plain main(); don't pull in SDL2main's WinMain shim
#include <SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../core/ss_layout.h"
#include "../core/ss_join.h"
#include "../core/ss_profile.h"

// Host-wide config resolved from argv/env at startup (mirrors the Win32 host's globals).
struct HostConfig {
    int         numPlayers = 1;
    const char* hookLib    = nullptr;   // engine capture library (ss_hook.dll / .dylib)
    const char* engineExe  = nullptr;   // zandronum(.exe)
    const char* iwad       = nullptr;
    const char* gamedir    = nullptr;
    const char* seatsCfg   = nullptr;   // preferred baseline (seats_preferred.cfg)
    const char* absoluteCfg = nullptr;  // absolute override (seats_absolute.cfg)
    bool        autoStart  = true;      // SS_AUTOSTART=0 -> wait-screen start
    bool        invMouseY  = false, invPadY = false, invPadX = false;
    char        kbmProfile[64] = "";    // SS_KBM_PROFILE: the saved profile seat 0 loads
};

static void ParseEnv(HostConfig& cfg) {
    cfg.invMouseY = getenv("SS_MOUSE_INVY") != nullptr;
    cfg.invPadY   = getenv("SS_PAD_INVY")   != nullptr;
    cfg.invPadX   = getenv("SS_PAD_INVX")   != nullptr;
    if (const char* as = getenv("SS_AUTOSTART"); as && as[0] == '0') cfg.autoStart = false;
    if (const char* kp = getenv("SS_KBM_PROFILE"); kp && kp[0]) {
        snprintf(cfg.kbmProfile, sizeof(cfg.kbmProfile), "%s", kp);
    }
}

int main(int argc, char** argv) {
    if (argc < 5) {
        printf("usage: host <numPlayers> <hook-lib> <zandronum> <iwad> [gamedir] [preferred.cfg] [absolute.cfg]\n");
        return 1;
    }
    HostConfig cfg;
    cfg.numPlayers  = atoi(argv[1]); if (cfg.numPlayers < 1) cfg.numPlayers = 1; if (cfg.numPlayers > 4) cfg.numPlayers = 4;
    cfg.hookLib     = argv[2];
    cfg.engineExe   = argv[3];
    cfg.iwad        = argv[4];
    cfg.gamedir     = (argc > 5) ? argv[5] : nullptr;
    cfg.seatsCfg    = (argc > 6) ? argv[6] : nullptr;
    cfg.absoluteCfg = (argc > 7) ? argv[7] : nullptr;
    ParseEnv(cfg);

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 2;
    }
    SDL_Window* win = SDL_CreateWindow("splitdronum",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) { fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError()); SDL_Quit(); return 3; }
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) { fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError()); SDL_DestroyWindow(win); SDL_Quit(); return 4; }

    // Main loop scaffold: the compositor, input routing, and the manager/join glue land here in the
    // staged port. For now this proves the SDL window + GPU renderer come up and the loop pumps cleanly.
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) running = false;
        }
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderPresent(ren);
    }

    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
