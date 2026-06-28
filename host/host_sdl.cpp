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
#include <cstdint>
#include <vector>
#include <mutex>
#include "../core/ss_layout.h"
#include "../core/ss_join.h"
#include "../core/ss_profile.h"
#include "ss_shm.h"

// --- shared-memory protocol (must match the engine capture hook) -----------------------------------
// Framebuffer "ZanDLLFB_<pid>": int[0]=magic, [1]=w, [2]=h, [3]=frame; then BGRA pixels bottom-up at
// byte offset 16. Input "ZanIN_<pid>": int[0]=magic, [3]=mouse dx, [4]=mouse dy, [5]=key write-count;
// a ring of 64 key events at [8]; 5 analog axes at [136]; the DLL writes [160]=1 when that seat is in a
// menu; a console-command channel at [164]/[168].
static const int32_t FB_MAGIC = 0x5A444C46;   // 'FLDZ'
static const int32_t IN_MAGIC = 0x5A414E49;   // 'ZANI'
static const int FB_PIXELS_OFF = 16;          // bytes: header is 4 ints, then pixels
static const int KEY_RING   = 64;
static const int IN_MOUSE_DX = 3, IN_MOUSE_DY = 4, IN_KEYCOUNT = 5, IN_RING = 8;
static const int IN_AXES    = 136;            // 5 axes (value x1000)
static const int IN_MENU    = 160;            // DLL writes 1 = seat in a menu/console
static const int IN_CMDSEQ  = 164;            // bumped after a console command is written
static const int IN_CMDTEXT = 168;            // command string bytes
static const int IN_CMDMAX  = 120;
static const int IN_BYTES   = 1024;           // input section size
enum { AX_Yaw = 0, AX_Pitch = 1, AX_Forward = 2, AX_Side = 3 };   // m_joy.h order

// --- a composited client seat (portable; mirrors the Win32 host's Client) --------------------------
struct Client {
    uint32_t       pid   = 0;
    ss::ShmView    fb;                       // framebuffer (read-only)
    const uint8_t* view  = nullptr;          // fb.ptr as bytes
    ss::ShmView    inShm;                     // input channel (read/write)
    volatile int32_t* in = nullptr;          // inShm.ptr as ints
    int   keyWrite = 0;
    bool  alive    = true;
    int   pad      = -1;                      // SDL controller index driving this seat; -1 = seat 0 (kbd/mouse)
    char  name[48] = "";                      // this seat's profile/player name (no-duplicate-profile check)
    SDL_Texture* tex = nullptr;               // cached GPU texture for this seat's framebuffer
    int   texW = 0, texH = 0;                 // its current dimensions (recreated on a render-res change)
};

static std::vector<Client> g_clients;
static std::mutex          g_clientsLock;     // guards g_clients (manager mutates / render+input read)
static bool g_autoStart = true;               // SS_AUTOSTART=0 -> wait-screen start
static int32_t g_launchN = 0;                 // seats spawned so far (drives the loading/prompt text)

static int LiveCount() { int n = 0; for (auto& c : g_clients) if (c.alive) ++n; return n; }

// --- input-ring writers (host -> client), matching the engine hook's reader -----------------------
// push one key event: a = (scancode<<1)|down, b = guiKey | (char<<16). The DLL reads [5] last.
static void PushKey(Client& c, int sc, int gk, int ch, int down) {
    if (!c.in) return;
    int slot = c.keyWrite % KEY_RING;
    c.in[IN_RING + slot * 2]     = (sc << 1) | (down & 1);
    c.in[IN_RING + slot * 2 + 1] = (gk & 0xffff) | (ch << 16);
    ++c.keyWrite;
    c.in[IN_KEYCOUNT] = c.keyWrite;
}
// post a controller button as a RAW EV_Key event (raw bit set) so the engine routes it through its own
// controller binds, in game and in menus.
static void PushButton(Client& c, int code, int down) {
    if (!c.in) return;
    int slot = c.keyWrite % KEY_RING;
    c.in[IN_RING + slot * 2]     = (code << 1) | (down & 1) | (1 << 16);   // raw bit (bit 16)
    c.in[IN_RING + slot * 2 + 1] = 0;
    ++c.keyWrite;
    c.in[IN_KEYCOUNT] = c.keyWrite;
}
// accumulate a relative mouse delta (the DLL consumes + zeroes each frame)
static void PushMouse(Client& c, int dx, int dy) {
    if (!c.in) return;
    c.in[IN_MOUSE_DX] = c.in[IN_MOUSE_DX] + dx;
    c.in[IN_MOUSE_DY] = c.in[IN_MOUSE_DY] + dy;
}
// write a native joystick axis (value scaled x1000)
static void WriteAxis(Client& c, int idx, float v) {
    if (c.in) c.in[IN_AXES + idx] = (int32_t)(v * 1000.0f);
}
// send a console command to a running seat: write the text, then bump the sequence so the DLL runs it once.
static void WriteCommand(Client& c, const char* cmd) {
    if (!c.in) return;
    char* dst = (char*)(c.in + IN_CMDTEXT);
    snprintf(dst, IN_CMDMAX, "%s", cmd);
    c.in[IN_CMDSEQ] = c.in[IN_CMDSEQ] + 1;   // publish AFTER the text is in place
}

// open a seat's framebuffer section if not already mapped (the client creates it once it's up)
static void OpenFb(Client& c) {
    if (c.view) return;
    char nm[64]; snprintf(nm, sizeof(nm), "ZanDLLFB_%u", c.pid);
    c.fb = ss::ShmOpenRead(nm);
    c.view = (const uint8_t*)c.fb.ptr;
}
// create this seat's host->client input section ("ZanIN_<pid>")
static void CreateSeatIn(Client& c) {
    char nm[64]; snprintf(nm, sizeof(nm), "ZanIN_%u", c.pid);
    c.inShm = ss::ShmCreate(nm, IN_BYTES);
    c.in = (volatile int32_t*)c.inShm.ptr;
    if (c.in) c.in[0] = IN_MAGIC;
}

// --- compositor: composite every live seat's framebuffer into the window via core/ layout ----------
// Each seat's BGRA bottom-up framebuffer is uploaded to a streaming texture (recreated only on a
// render-res change) and drawn, letterboxed, into its pane -- V-flipped at draw time since the source
// is bottom-up (glReadPixels order). SDL_Renderer does the GPU scale + flip.
static void Render(SDL_Renderer* ren, int W, int H) {
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);

    std::lock_guard<std::mutex> lk(g_clientsLock);
    int live = LiveCount();
    ss::Layout lay = ss::ComputeLayout(live, W, H, ss::TwoMode::Auto);
    bool drew = false;
    int pane = 0;
    for (size_t i = 0; i < g_clients.size() && (int)i < 4; ++i) {
        Client& c = g_clients[i];
        if (!c.alive || pane >= lay.count) continue;
        OpenFb(c);
        const int32_t* hdr = c.view ? (const int32_t*)c.view : nullptr;
        if (hdr && hdr[0] == FB_MAGIC && hdr[1] > 0 && hdr[2] > 0) {
            int fw = hdr[1], fh = hdr[2];
            if (!c.tex || c.texW != fw || c.texH != fh) {            // (re)create at the seat's render res
                if (c.tex) SDL_DestroyTexture(c.tex);
                c.tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,   // BGRA bytes (little-endian)
                                          SDL_TEXTUREACCESS_STREAMING, fw, fh);
                c.texW = fw; c.texH = fh;
            }
            if (c.tex) {
                SDL_UpdateTexture(c.tex, nullptr, c.view + FB_PIXELS_OFF, fw * 4);
                ss::Rect b = ss::Letterbox(lay.panes[pane], fw, fh);
                SDL_Rect dst = { b.x, b.y, b.w, b.h };
                SDL_RenderCopyEx(ren, c.tex, nullptr, &dst, 0.0, nullptr, SDL_FLIP_VERTICAL);   // bottom-up source
                drew = true;
            }
        }
        pane++;
    }

    if (!drew) {
        // Nothing rendering yet: a flat dark field stands in for the prompt until the SDL overlay lands
        // (next stage). The Win32 host draws "press any key / Starting..." here.
        SDL_SetRenderDrawColor(ren, 16, 18, 24, 255);
        SDL_Rect full = { 0, 0, W, H };
        SDL_RenderFillRect(ren, &full);
    }
    SDL_RenderPresent(ren);
}

// Host-wide config resolved from argv/env at startup (mirrors the Win32 host's globals).
struct HostConfig {
    int         numPlayers = 1;
    const char* hookLib    = nullptr;   // engine capture library (ss_hook.dll / .dylib)
    const char* engineExe  = nullptr;   // zandronum(.exe)
    const char* iwad       = nullptr;
    const char* gamedir    = nullptr;
    const char* seatsCfg   = nullptr;   // preferred baseline (seats_preferred.cfg)
    const char* absoluteCfg = nullptr;  // absolute override (seats_absolute.cfg)
    bool        invMouseY  = false, invPadY = false, invPadX = false;
    char        kbmProfile[64] = "";    // SS_KBM_PROFILE: the saved profile seat 0 loads
};

static void ParseEnv(HostConfig& cfg) {
    cfg.invMouseY = getenv("SS_MOUSE_INVY") != nullptr;
    cfg.invPadY   = getenv("SS_PAD_INVY")   != nullptr;
    cfg.invPadX   = getenv("SS_PAD_INVX")   != nullptr;
    if (const char* as = getenv("SS_AUTOSTART"); as && as[0] == '0') g_autoStart = false;
    if (const char* kp = getenv("SS_KBM_PROFILE"); kp && kp[0])
        snprintf(cfg.kbmProfile, sizeof(cfg.kbmProfile), "%s", kp);
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

    // Main loop: pump events, composite. Input routing + the manager/join glue land in the next stages
    // (they run on their own threads against g_clients, exactly like the Win32 host).
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) running = false;
        }
        int W = 0, H = 0;
        SDL_GetRendererOutputSize(ren, &W, &H);
        if (W > 0 && H > 0) Render(ren, W, H);
        else SDL_Delay(8);
    }

    {
        std::lock_guard<std::mutex> lk(g_clientsLock);
        for (auto& c : g_clients) { if (c.tex) SDL_DestroyTexture(c.tex); ss::ShmClose(c.fb); ss::ShmClose(c.inShm); }
    }
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
