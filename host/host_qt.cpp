// host_qt -- the cross-platform splitscreen host/compositor (Windows + macOS).
//
// This is the multiplatform replacement for host/host.cpp (the legacy Win32/D3D11 host). It does the
// same job -- launch N stock Zandronum clients, read each client's framebuffer from shared memory, and
// composite them into ONE window with the QML overlay on top -- but on a portable stack:
//
//   window + compositor + overlay : Qt Quick. One QML scene draws the seat framebuffers AND the overlay,
//                                   on Qt's RHI (D3D11 on Windows, Metal on macOS) -- no GPU-specific code.
//   gamepads                      : SDL2 (gamecontroller subsystem only -- Qt 6 has no gamepad module).
//                                   No SDL window or renderer.
//   keyboard / mouse              : Qt input events.
//   shared memory                 : ss_shm.h shim (Win32 file-mapping / POSIX shm_open+mmap).
//   client launch + inject        : ss_proc.h shim (Win32 CreateProcess+CreateRemoteThread / POSIX
//                                   posix_spawn + DYLD_INSERT_LIBRARIES).
//
// All decision logic (layout, the controller join state machine, profiles) is the pure, fully tested
// core/ library -- shared verbatim with the Win32 host. Only the platform glue differs.
//
// Build: enabled by -DSS_BUILD_HOST=ON (see CMakeLists.txt). The CI matrix compiles + links it on both
// Windows and macOS; runtime parity on macOS additionally needs the engine-side capture hook ported.
//
//   host <numPlayers> <hook-lib> <zandronum> <iwad> [gamedir] [seats_preferred.cfg] [seats_absolute.cfg]
#define SDL_MAIN_HANDLED            // we provide a plain main(); don't pull in SDL2main's WinMain shim
#include <SDL.h>
#include <QGuiApplication>
#include <QQuickView>
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
    int   pad      = -1;                      // SDL controller slot driving this seat; -1 = seat 0 (kbd/mouse)
    char  name[48] = "";                      // this seat's profile/player name (no-duplicate-profile check)
};

static std::vector<Client> g_clients;
static std::mutex          g_clientsLock;     // guards g_clients (manager mutates / render+input read)
static bool g_autoStart = true;               // SS_AUTOSTART=0 -> wait-screen start
static int32_t g_launchN = 0;                 // seats spawned so far (drives the loading/prompt text)
static bool g_invMouseY = false, g_invPadY = false, g_invPadX = false;   // optional per-axis look invert
static SDL_GameController* g_pads[4] = { nullptr, nullptr, nullptr, nullptr };   // open pads by slot 0..3
static bool g_mouseFreed = false;             // a menu is up -> stop feeding seat 0 look, free the cursor

static int LiveCount() { int n = 0; for (auto& c : g_clients) if (c.alive) ++n; return n; }
static bool PadConnected(int p) { return p >= 0 && p < 4 && g_pads[p] != nullptr; }

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

// --- controller gameplay feed (SDL) -------------------------------------------------------------
// normalized stick value (-1..1), 0 inside the deadzone (axis is +/-32767)
static float StickAxis(int v, int dz) {
    if (v >  dz) return (float)(v - dz) / (32767 - dz);
    if (v < -dz) return (float)(v + dz) / (32767 - dz);
    return 0.f;
}
// SDL controller button -> the engine pad-key code the Win32 host used (XInput bit order: 0x1B4 + bit),
// so existing controller binds in splitseat cfgs match on both hosts.
static int PadButtonCode(SDL_GameControllerButton b) {
    switch (b) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:        return 0x1B4 + 0;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:      return 0x1B4 + 1;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:      return 0x1B4 + 2;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:     return 0x1B4 + 3;
        case SDL_CONTROLLER_BUTTON_START:          return 0x1B4 + 4;
        case SDL_CONTROLLER_BUTTON_BACK:           return 0x1B4 + 5;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK:      return 0x1B4 + 6;
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK:     return 0x1B4 + 7;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:   return 0x1B4 + 8;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:  return 0x1B4 + 9;
        case SDL_CONTROLLER_BUTTON_A:              return 0x1B4 + 12;
        case SDL_CONTROLLER_BUTTON_B:              return 0x1B4 + 13;
        case SDL_CONTROLLER_BUTTON_X:              return 0x1B4 + 14;
        case SDL_CONTROLLER_BUTTON_Y:              return 0x1B4 + 15;
        default: return 0;
    }
}
// Per-pad edge-detect state so a held button isn't re-sent every frame.
struct PadEdge { uint16_t prevBtn = 0; bool lt = false, rt = false; };
static PadEdge g_padEdge[4];
// Drive every controller seat from its assigned pad: native analog axes + edge-detected buttons. Called
// each tick after SDL_GameControllerUpdate() refreshes pad state.
static void FeedGameplay() {
    std::lock_guard<std::mutex> lk(g_clientsLock);
    for (auto& c : g_clients) {
        if (!c.alive || c.pad < 0 || c.pad >= 4 || !g_pads[c.pad]) continue;
        SDL_GameController* gc = g_pads[c.pad];
        // SDL Y axes are +down; convert to XInput's +up convention so the core movement signs match.
        int lx =  SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
        int ly = -SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
        int rx =  SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX);
        int ry = -SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY);
        const int DZ = 7000;
        WriteAxis(c, AX_Forward,  StickAxis(ly, DZ));                 // left stick = move (analog)
        WriteAxis(c, AX_Side,    -StickAxis(lx, DZ));
        WriteAxis(c, AX_Yaw,   (g_invPadX ?  1.f : -1.f) * StickAxis(rx, DZ));   // right stick = look
        WriteAxis(c, AX_Pitch, (g_invPadY ? -1.f :  1.f) * StickAxis(ry, DZ));
        PadEdge& s = g_padEdge[c.pad];
        uint16_t btn = 0;
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b)
            if (SDL_GameControllerGetButton(gc, (SDL_GameControllerButton)b)) btn |= (uint16_t)(1 << b);
        uint16_t changed = btn ^ s.prevBtn;
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b)
            if (changed & (1 << b)) { int code = PadButtonCode((SDL_GameControllerButton)b); if (code) PushButton(c, code, (btn >> b) & 1); }
        s.prevBtn = btn;
        bool lt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  > 8192;
        bool rt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8192;
        if (lt != s.lt) { PushButton(c, 0x1BE, lt ? 1 : 0); s.lt = lt; }   // KEY_PAD_LTRIGGER
        if (rt != s.rt) { PushButton(c, 0x1BF, rt ? 1 : 0); s.rt = rt; }   // KEY_PAD_RTRIGGER
    }
}

// Open/close pads as SDL reports them, into a stable slot 0..3 (the manager binds a slot to a seat).
static void OnControllerAdded(int deviceIndex) {
    if (!SDL_IsGameController(deviceIndex)) return;
    for (int p = 0; p < 4; ++p) if (!g_pads[p]) { g_pads[p] = SDL_GameControllerOpen(deviceIndex); return; }
}
static void OnControllerRemoved(SDL_JoystickID instance) {
    for (int p = 0; p < 4; ++p) {
        if (!g_pads[p]) continue;
        SDL_Joystick* js = SDL_GameControllerGetJoystick(g_pads[p]);
        if (js && SDL_JoystickInstanceID(js) == instance) { SDL_GameControllerClose(g_pads[p]); g_pads[p] = nullptr; g_padEdge[p] = PadEdge(); return; }
    }
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
    char        kbmProfile[64] = "";    // SS_KBM_PROFILE: the saved profile seat 0 loads
};

static void ParseEnv(HostConfig& cfg) {
    g_invMouseY = getenv("SS_MOUSE_INVY") != nullptr;
    g_invPadY   = getenv("SS_PAD_INVY")   != nullptr;
    g_invPadX   = getenv("SS_PAD_INVX")   != nullptr;
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

    // SDL is used ONLY for gamepads -- no video subsystem, no window. SDL_GameControllerUpdate() (polled
    // by the manager) refreshes state; events drive hotplug (open/close in the manager loop).
    SDL_SetMainReady();
    SDL_Init(SDL_INIT_GAMECONTROLLER);

    QGuiApplication app(argc, argv);

    // Qt Quick scene: the seat compositor + overlay live here. The full scene QML + SeatItem land in the
    // compositor stage; for now bring up the window so the Qt + SDL + core build links on both OSes.
    QQuickView view;
    view.setTitle("splitdronum");
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.resize(1280, 720);
    view.show();

    int rc = app.exec();

    {
        std::lock_guard<std::mutex> lk(g_clientsLock);
        for (auto& c : g_clients) { ss::ShmClose(c.fb); ss::ShmClose(c.inShm); }
    }
    for (int p = 0; p < 4; ++p) if (g_pads[p]) SDL_GameControllerClose(g_pads[p]);
    SDL_Quit();
    return rc;
}
