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

// Winsock must be included BEFORE windows.h (pulled in by ss_shm.h / ss_proc.h / SDL / Qt) to avoid the
// winsock1 clash; including it first defines _WINSOCKAPI_ so later windows.h won't re-include winsock1.
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <unistd.h>
  #include <dirent.h>
  #include <sys/stat.h>
  #include <strings.h>
#endif

#define SDL_MAIN_HANDLED            // we provide a plain main(); don't pull in SDL2main's WinMain shim
#include <SDL.h>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QQuickItem>
#include <QSGImageNode>
#include <QImage>
#include <QTimer>
#include <QColor>
#include <QScreen>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QCursor>
#include <QPoint>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QUrl>
#include <QVariant>
#include <QVariantMap>
#include <QVariantList>
#include <QFileInfo>
#include <QStringList>
#include <QCoreApplication>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include "../core/ss_layout.h"
#include "../core/ss_join.h"
#include "../core/ss_profile.h"
#include "../core/ss_input.h"
#include "ss_shm.h"
#include "ss_proc.h"

#ifdef _WIN32
  typedef SOCKET sock_t;
  #define SS_CLOSESOCK closesocket
  #define SS_BADSOCK   INVALID_SOCKET
#else
  typedef int sock_t;
  #define SS_CLOSESOCK close
  #define SS_BADSOCK   (-1)
#endif

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

// --- host-wide config resolved from argv/env at startup --------------------------------------------
struct HostConfig {
    int         numPlayers = 1;
    const char* hookLib    = nullptr;   // engine capture library (ss_hook.dll / .dylib)
    const char* engineExe  = nullptr;   // zandronum(.exe)
    const char* iwad       = nullptr;
    const char* gamedir    = ".";       // cwd for clients + where cfg/profiles live
    const char* seatsCfg   = nullptr;   // preferred baseline (seats_preferred.cfg)
    const char* absoluteCfg = nullptr;  // absolute override (seats_absolute.cfg)
    char        kbmProfile[64] = "";    // SS_KBM_PROFILE: the saved profile seat 0 loads
};
static HostConfig g_cfg;
static int g_port = 10666;              // loopback server/connect port; resolved to a free one at startup
static int g_screenW = 1920, g_screenH = 1080;   // captured from QScreen on the GUI thread (DesiredRes)

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
static bool g_enterDown = false;              // Enter currently held (keyboard hold-to-join), via Qt events
static bool g_kbmEverUp = false;              // a kbd/mouse seat 0 has existed (gate the "seat 0 left" prompt)

// Create-and-join flow state, published by the manager step for the overlay (all touched on the GUI thread).
static int  g_joinPad = -1, g_joinField = 0, g_joinW1 = 0, g_joinW2 = 0, g_joinCross = 0, g_joinMotion = 0;
static int  g_joinTaken = 0, g_joinKnown = 0, g_joinHold = 0, g_kbHold = 0, g_joinMode = 0, g_browseIndex = 0;
static char g_joinVariant[64] = "";
static ss::ProfileList g_profiles;            // saved configs the browser lists (refreshed on open)
// Seat-0 keys/buttons the host has currently sent DOWN (held[sc] != 0). Lets the driver auto-release a key
// that got stuck -- e.g. alt-tab (or cmd-tab on macOS) sends Alt-down (=+strafe) but focus leaves before
// the Alt-up, leaving strafe held so mouse-X strafes instead of turning. Touched on the GUI thread only.
static unsigned char g_seat0Down[0x200];

static int LiveCount() { int n = 0; for (auto& c : g_clients) if (c.alive) ++n; return n; }
static bool PadConnected(int p) { return p >= 0 && p < 4 && g_pads[p] != nullptr; }
static int StrCaseCmp(const char* a, const char* b) {
#ifdef _WIN32
    return _stricmp(a, b);
#else
    return strcasecmp(a, b);
#endif
}

// --- input-ring writers (host -> client), matching the engine hook's reader -----------------------
static void PushKey(Client& c, int sc, int gk, int ch, int down) {
    if (!c.in) return;
    int slot = c.keyWrite % KEY_RING;
    c.in[IN_RING + slot * 2]     = (sc << 1) | (down & 1);
    c.in[IN_RING + slot * 2 + 1] = (gk & 0xffff) | (ch << 16);
    ++c.keyWrite;
    c.in[IN_KEYCOUNT] = c.keyWrite;
}
static void PushButton(Client& c, int code, int down) {
    if (!c.in) return;
    int slot = c.keyWrite % KEY_RING;
    c.in[IN_RING + slot * 2]     = (code << 1) | (down & 1) | (1 << 16);   // raw bit (bit 16)
    c.in[IN_RING + slot * 2 + 1] = 0;
    ++c.keyWrite;
    c.in[IN_KEYCOUNT] = c.keyWrite;
}
static void PushMouse(Client& c, int dx, int dy) {
    if (!c.in) return;
    c.in[IN_MOUSE_DX] = c.in[IN_MOUSE_DX] + dx;
    c.in[IN_MOUSE_DY] = c.in[IN_MOUSE_DY] + dy;
}
static void WriteAxis(Client& c, int idx, float v) {
    if (c.in) c.in[IN_AXES + idx] = (int32_t)(v * 1000.0f);
}
static void WriteCommand(Client& c, const char* cmd) {
    if (!c.in) return;
    char* dst = (char*)(c.in + IN_CMDTEXT);
    snprintf(dst, IN_CMDMAX, "%s", cmd);
    c.in[IN_CMDSEQ] = c.in[IN_CMDSEQ] + 1;   // publish AFTER the text is in place
}

static void OpenFb(Client& c) {
    if (c.view) return;
    char nm[64]; snprintf(nm, sizeof(nm), "ZanDLLFB_%u", c.pid);
    c.fb = ss::ShmOpenRead(nm);
    c.view = (const uint8_t*)c.fb.ptr;
}
static void CreateSeatIn(Client& c) {
    char nm[64]; snprintf(nm, sizeof(nm), "ZanIN_%u", c.pid);
    c.inShm = ss::ShmCreate(nm, IN_BYTES);
    c.in = (volatile int32_t*)c.inShm.ptr;
    if (c.in) c.in[0] = IN_MAGIC;
}
static void UnmapSeat(Client& c) {
    ss::ShmClose(c.fb); c.view = nullptr;
    ss::ShmClose(c.inShm); c.in = nullptr;
}

// --- controller gameplay feed (SDL) -------------------------------------------------------------
static float StickAxis(int v, int dz) {
    if (v >  dz) return (float)(v - dz) / (32767 - dz);
    if (v < -dz) return (float)(v + dz) / (32767 - dz);
    return 0.f;
}
// SDL controller button -> the XInput bit position the Win32 host used; the engine pad-key code is
// 0x1B4 + bit, and ss_join maps the XInput button mask, so binds match across both hosts.
static int PadXInputBit(SDL_GameControllerButton b) {
    switch (b) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:        return 0;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:      return 1;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:      return 2;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:     return 3;
        case SDL_CONTROLLER_BUTTON_START:          return 4;
        case SDL_CONTROLLER_BUTTON_BACK:           return 5;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK:      return 6;
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK:     return 7;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:   return 8;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:  return 9;
        case SDL_CONTROLLER_BUTTON_A:              return 12;
        case SDL_CONTROLLER_BUTTON_B:              return 13;
        case SDL_CONTROLLER_BUTTON_X:              return 14;
        case SDL_CONTROLLER_BUTTON_Y:              return 15;
        default: return -1;
    }
}
// Current buttons as an XInput-style bitmask (1<<bit). Used both for gameplay and to feed ss_join.
static int SdlXInputMask(SDL_GameController* gc) {
    int m = 0;
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b) {
        if (SDL_GameControllerGetButton(gc, (SDL_GameControllerButton)b)) {
            int bit = PadXInputBit((SDL_GameControllerButton)b);
            if (bit >= 0) m |= (1 << bit);
        }
    }
    return m;
}
struct PadEdge { uint16_t prevBtn = 0; bool lt = false, rt = false; };
static PadEdge g_padEdge[4];
static uint32_t g_backHold[4] = { 0, 0, 0, 0 };   // BACK held-since timestamp per pad (hold-to-leave)
static void EnqueueDespawn(uint32_t pid);          // fwd: drop-out kills the seat's process on the worker
static void FeedGameplay() {
    std::lock_guard<std::mutex> lk(g_clientsLock);
    for (auto& c : g_clients) {
        if (!c.alive || c.pad < 0 || c.pad >= 4 || !g_pads[c.pad]) continue;
        SDL_GameController* gc = g_pads[c.pad];
        // Hold BACK ~1.2s to leave: the seat despawns and its pad frees.
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK)) {
            if (!g_backHold[c.pad]) g_backHold[c.pad] = SDL_GetTicks();
            else if (SDL_GetTicks() - g_backHold[c.pad] > 1200) {
                g_backHold[c.pad] = 0; uint32_t pid = c.pid;
                c.alive = false; UnmapSeat(c); c.pid = 0;
                if (pid) EnqueueDespawn(pid);
                continue;
            }
        } else g_backHold[c.pad] = 0;
        int lx =  SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
        int ly = -SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);   // SDL Y is +down -> flip to +up
        int rx =  SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX);
        int ry = -SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY);
        const int DZ = 7000;
        WriteAxis(c, AX_Forward,  StickAxis(ly, DZ));
        WriteAxis(c, AX_Side,    -StickAxis(lx, DZ));
        WriteAxis(c, AX_Yaw,   (g_invPadX ?  1.f : -1.f) * StickAxis(rx, DZ));
        WriteAxis(c, AX_Pitch, (g_invPadY ? -1.f :  1.f) * StickAxis(ry, DZ));
        PadEdge& s = g_padEdge[c.pad];
        uint16_t btn = (uint16_t)SdlXInputMask(gc);
        uint16_t changed = btn ^ s.prevBtn;
        for (int bit = 0; bit < 16; ++bit)
            if (changed & (1 << bit)) PushButton(c, 0x1B4 + bit, (btn >> bit) & 1);   // KEY_PAD_* + bit
        s.prevBtn = btn;
        bool lt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  > 8192;
        bool rt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8192;
        if (lt != s.lt) { PushButton(c, 0x1BE, lt ? 1 : 0); s.lt = lt; }   // KEY_PAD_LTRIGGER
        if (rt != s.rt) { PushButton(c, 0x1BF, rt ? 1 : 0); s.rt = rt; }   // KEY_PAD_RTRIGGER
    }
}
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

// --- cross-platform filesystem + socket helpers --------------------------------------------------
static void MakeDir(const char* path) {
#ifdef _WIN32
    CreateDirectoryA(path, NULL);
#else
    mkdir(path, 0755);
#endif
}
// True if INADDR_ANY:port can't be bound right now (someone holds it). Matches the stock server's bind.
static bool PortInUse(int port) {
    sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == SS_BADSOCK) return false;
    sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons((unsigned short)port); a.sin_addr.s_addr = htonl(INADDR_ANY);
    int r = bind(s, (sockaddr*)&a, sizeof(a));
    SS_CLOSESOCK(s);
    return r != 0;
}
static int PickFreeUdpPort(int start) {
    for (int p = start; p < start + 64; ++p) if (!PortInUse(p)) return p;
    return start;
}
// Scan <gamedir>/profiles/*.cfg into a ss::ProfileList (display names) for the load-existing browser.
static void EnumerateProfiles(ss::ProfileList& list) {
    list.count = 0;
    char dir[600]; snprintf(dir, sizeof(dir), "%s/profiles", g_cfg.gamedir);
#ifdef _WIN32
    char glob[700]; snprintf(glob, sizeof(glob), "%s/*.cfg", dir);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(glob, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) ss::ProfileAddFromFile(list, fd.cFileName); }
    while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d))) {
        const char* n = e->d_name;
        size_t len = strlen(n);
        if (len > 4 && strcmp(n + len - 4, ".cfg") == 0) ss::ProfileAddFromFile(list, n);
    }
    closedir(d);
#endif
}

// --- per-seat cfg deployment + profile I/O (fopen-based; portable) --------------------------------
static void DeployOneCfg(const char* src, const char* destName, const char* fallback) {
    char path[700]; snprintf(path, sizeof(path), "%s/%s", g_cfg.gamedir, destName);
    FILE* out = fopen(path, "w");
    if (!out) return;
    FILE* in = src ? fopen(src, "rb") : nullptr;
    if (in) {
        char buf[2048]; size_t r;
        while ((r = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, r, out);
        fclose(in);
    } else if (fallback) {
        fputs(fallback, out);
    }
    fclose(out);
}
static void DeploySeatCfg() {
    DeployOneCfg(g_cfg.seatsCfg, "splitseat.cfg", nullptr);   // PREFERRED baseline (profiles override it)
    DeployOneCfg(g_cfg.absoluteCfg, "splitseat_absolute.cfg",  // ABSOLUTE override (execed last -> wins)
                 "bind pad_b \"ifspectator menu_join\"\nbind pad_x \"ifspectator menu_join\"\n");
}
static void WriteProfileCfg(const char* name, int crosshair, int motionComp) {
    char dir[600]; snprintf(dir, sizeof(dir), "%s/profiles", g_cfg.gamedir); MakeDir(dir);
    char path[800]; snprintf(path, sizeof(path), "%s/%s.cfg", dir, name);
    char body[512]; ss::ProfileBuildCfg(body, sizeof(body), name, crosshair, motionComp);
    FILE* f = fopen(path, "w");
    if (f) { fputs(body, f); fclose(f); }
}
static bool LoadProfileCfg(const char* name, int* crosshair, int* motionComp) {
    char path[800]; snprintf(path, sizeof(path), "%s/profiles/%s.cfg", g_cfg.gamedir, name);
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char body[1024]; size_t n = fread(body, 1, sizeof(body) - 1, f); body[n] = '\0';
    fclose(f);
    return ss::ProfileParseCfg(body, crosshair, motionComp);
}
static void SaveSeatLast(int slot, int w1, int w2, int crosshair, int motion) {
    if (slot < 1) return;
    char path[800]; snprintf(path, sizeof(path), "%s/profiles/.seat%d.last", g_cfg.gamedir, slot);
    FILE* f = fopen(path, "w");
    if (f) { fprintf(f, "%d %d %d %d\n", w1, w2, crosshair, motion); fclose(f); }
}
static bool LoadSeatLast(int slot, ss::JoinState& js) {
    if (slot < 1) return false;
    char path[800]; snprintf(path, sizeof(path), "%s/profiles/.seat%d.last", g_cfg.gamedir, slot);
    FILE* f = fopen(path, "r");
    if (!f) return false;
    int w1, w2, c, m; bool ok = (fscanf(f, "%d %d %d %d", &w1, &w2, &c, &m) == 4); fclose(f);
    if (ok) { js.word1 = w1; js.word2 = w2; js.crosshair = c; js.motion = m; }
    return ok;
}

// --- seat bookkeeping (callers hold g_clientsLock unless noted) ------------------------------------
static bool NameInUse(const char* name) {
    bool used = false;
    std::lock_guard<std::mutex> lk(g_clientsLock);
    for (auto& c : g_clients) if (c.alive && StrCaseCmp(c.name, name) == 0) { used = true; break; }
    return used;
}
static int NextControllerSlot() {
    std::lock_guard<std::mutex> lk(g_clientsLock);
    int slot = -1;
    for (size_t i = 1; i < g_clients.size() && slot < 0; ++i) if (!g_clients[i].alive) slot = (int)i;
    if (slot < 0 && g_clients.size() < 4) { slot = (int)g_clients.size(); if (slot < 1) slot = 1; }
    return slot;
}
static bool Seat0Alive() {
    std::lock_guard<std::mutex> lk(g_clientsLock);
    return !g_clients.empty() && g_clients[0].alive && g_clients[0].pad < 0;
}
static int LiveCountLocked() { std::lock_guard<std::mutex> lk(g_clientsLock); return LiveCount(); }
static bool PadAssigned(int pad) {
    std::lock_guard<std::mutex> lk(g_clientsLock);
    for (auto& c : g_clients) if (c.alive && c.pad == pad) return true;
    return false;
}
// Names loaded by LIVE seats (so ProfileVariant picks a free variant); returns the count (<= cap).
static int LiveNames(char buf[][ss::kProfileNameLen], const char* ptrs[], int cap) {
    int n = 0;
    std::lock_guard<std::mutex> lk(g_clientsLock);
    for (auto& c : g_clients)
        if (c.alive && c.name[0] && n < cap) { snprintf(buf[n], ss::kProfileNameLen, "%s", c.name); ptrs[n] = buf[n]; ++n; }
    return n;
}
static void VariantNameFor(char* out, int cap, const char* base) {
    char buf[4][ss::kProfileNameLen]; const char* ptrs[4];
    int n = LiveNames(buf, ptrs, 4);
    ss::ProfileVariant(out, cap, base, ptrs, n);
}

// --- render resolution for a seat (monitor size, smart-scaled by seat count) ----------------------
static void DesiredRes(int liveCount, int& w, int& h) {
    int bw = g_screenW, bh = g_screenH;
    if (bw > 1920) bw = 1920;  if (bh > 1080) bh = 1080;   // bound per-client cost
    float s = ss::SmartScale(liveCount);
    w = (int)(bw * s);  h = (int)(bh * s);
    if (w < 640) w = 640;  if (h < 360) h = 360;
}

// --- spawn / despawn (run on the worker thread; process creation blocks) --------------------------
// Spawn one stock client, connect it to the loopback server, inject the capture lib, add it as a seat.
// pad = the SDL controller slot (-1 = seat 0, keyboard+mouse). profile = a profiles/<name>.cfg display
// name, or nullptr for the default P<seat> identity.
static bool SpawnClient(int pad, const char* profile) {
    int slot = -1, liveCount = 0;
    {
        std::lock_guard<std::mutex> lk(g_clientsLock);
        if (pad < 0) slot = 0;
        else {
            for (size_t i = 1; i < g_clients.size() && slot < 0; ++i) if (!g_clients[i].alive) slot = (int)i;
            if (slot < 0 && g_clients.size() < 4) { slot = (int)g_clients.size(); if (slot < 1) slot = 1; }
        }
        liveCount = LiveCount();
    }
    if (slot < 0 || slot > 3) return false;   // window full
    int seat = slot;
    int target = g_cfg.numPlayers > liveCount + 1 ? g_cfg.numPlayers : liveCount + 1;
    if (target > 4) target = 4;
    int mw, mh; DesiredRes(target, mw, mh);
    char args[820];
    ss::BuildClientArgs(args, sizeof(args), seat, g_cfg.iwad, g_port, mw, mh, profile);
    uint32_t pid = ss::ProcSpawn(g_cfg.engineExe, args, g_cfg.gamedir, g_cfg.hookLib);
    if (!pid) return false;
    Client c; c.pid = pid; c.pad = pad; c.alive = true;
    if (profile && profile[0]) snprintf(c.name, sizeof(c.name), "%s", profile);
    else                       snprintf(c.name, sizeof(c.name), "P%d", seat + 1);
    CreateSeatIn(c);
    {
        std::lock_guard<std::mutex> lk(g_clientsLock);
        while ((int)g_clients.size() <= slot) g_clients.push_back(Client());   // grow with dead placeholders
        g_clients[slot] = c;
    }
    ++g_launchN;
    if (pad < 0) g_kbmEverUp = true;   // the kbd/mouse seat has now existed -> rejoin prompt allowed
    return true;
}

// The dedicated server: probe a free port, host on it, and confirm it bound before any client connects.
static uint32_t g_serverPid = 0;
static void StartServer() {
    for (int attempt = 0; attempt < 4 && !g_serverPid; ++attempt) {
        g_port = PickFreeUdpPort(10666 + attempt);
        char srvArgs[600];
        snprintf(srvArgs, sizeof(srvArgs),
            "-host -iwad %s -port %d -config splitdronum-server.ini +map MAP01 +set sv_cooperative 1 "
            "+set sv_maxclients 8 +set sv_maxplayers 8 +set sv_maxclientsperip 8 +set sv_updatemaster 0 +set fullscreen 0",
            g_cfg.iwad, g_port);
        uint32_t pid = ss::ProcSpawn(g_cfg.engineExe, srvArgs, g_cfg.gamedir, nullptr);   // no injection
        bool up = false;
        for (int i = 0; i < 150 && !up; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); up = PortInUse(g_port); }
        if (up) g_serverPid = pid;
        else if (pid) ss::ProcKillForce(pid);
    }
}

// --- worker thread: performs the BLOCKING actions (server bring-up, spawn, despawn) off the GUI loop -
struct Action { int type; int pad; char profile[64]; uint32_t pid; };   // type 0 = spawn, 1 = despawn
static std::deque<Action>     g_actions;
static std::mutex             g_actMx;
static std::condition_variable g_actCv;
static bool                   g_quit = false;

static void EnqueueSpawn(int pad, const char* profile) {
    Action a; a.type = 0; a.pad = pad; a.pid = 0;
    snprintf(a.profile, sizeof(a.profile), "%s", profile ? profile : "");
    { std::lock_guard<std::mutex> lk(g_actMx); g_actions.push_back(a); }
    g_actCv.notify_one();
}
static void EnqueueDespawn(uint32_t pid) {
    Action a; a.type = 1; a.pad = -1; a.pid = pid; a.profile[0] = '\0';
    { std::lock_guard<std::mutex> lk(g_actMx); g_actions.push_back(a); }
    g_actCv.notify_one();
}
static void WorkerMain() {
    StartServer();
    if (g_autoStart) {
        SpawnClient(-1, g_cfg.kbmProfile[0] ? g_cfg.kbmProfile : nullptr);
        int initial = g_cfg.numPlayers; if (initial > 4) initial = 4;
        for (int s = 1; s < initial; ++s) SpawnClient(s - 1, nullptr);   // CLI -Players N: pre-assign pads
    }
    for (;;) {
        Action a;
        {
            std::unique_lock<std::mutex> lk(g_actMx);
            g_actCv.wait(lk, [] { return g_quit || !g_actions.empty(); });
            if (g_quit && g_actions.empty()) break;
            a = g_actions.front(); g_actions.pop_front();
        }
        if (a.type == 0) SpawnClient(a.pad, a.profile[0] ? a.profile : nullptr);
        else if (a.type == 1) ss::ProcKillGraceful(a.pid);
    }
}

// --- lifecycle ticks (GUI thread; quick) ---------------------------------------------------------
// Free any seat whose process exited on its own (in-game "quit game" or a crash) so its slot/pad frees.
static void ReapDeadSeats() {
    std::lock_guard<std::mutex> lk(g_clientsLock);
    for (auto& c : g_clients)
        if (c.alive && c.pid && !ss::ProcAlive(c.pid)) { UnmapSeat(c); c.pid = 0; c.alive = false; }
}
// Exactly one seat plays music: seat 0 while alive, else the lowest-index live controller seat. Hand off
// via the command channel when ownership changes (re-apply preferred then absolute so absolute still wins).
static int g_musicOwner = -1;
static void UpdateMusicOwner() {
    std::lock_guard<std::mutex> lk(g_clientsLock);
    int desired = -1;
    bool seat0 = !g_clients.empty() && g_clients[0].alive && g_clients[0].pad < 0;
    if (!seat0)
        for (size_t i = 1; i < g_clients.size(); ++i) if (g_clients[i].alive) { desired = (int)i; break; }
    if (desired != g_musicOwner) {
        if (g_musicOwner >= 1 && g_musicOwner < (int)g_clients.size() && g_clients[g_musicOwner].alive)
            WriteCommand(g_clients[g_musicOwner], "snd_musicvolume 0");
        if (desired >= 0) WriteCommand(g_clients[desired], "exec splitseat.cfg; exec splitseat_absolute.cfg");
        g_musicOwner = desired;
    }
}

// --- Windows desktop guards (no-ops elsewhere) --------------------------------------------------
// Parity with the legacy Win32 host: restore the desktop gamma the engine clobbers, keep the stock
// clients' own windows hidden (the DLL hides them at create; this is the per-frame catch), and reclaim
// foreground from any child window that steals it during the staggered startup.
#ifdef _WIN32
static WORD g_origGamma[768];
static bool g_gammaSaved = false;
static void SaveGamma() {
    HDC dc = GetDC(NULL);
    if (dc) { g_gammaSaved = !!GetDeviceGammaRamp(dc, g_origGamma); ReleaseDC(NULL, dc); }
}
static void RestoreGamma() {
    if (!g_gammaSaved) return;
    HDC dc = GetDC(NULL);
    if (dc) { SetDeviceGammaRamp(dc, g_origGamma); ReleaseDC(NULL, dc); }
}
static BOOL CALLBACK HideEnum_(HWND h, LPARAM lp) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    auto* pids = (std::vector<DWORD>*)lp;
    for (DWORD p : *pids) if (p == pid && IsWindowVisible(h)) { ShowWindow(h, SW_HIDE); break; }
    return TRUE;
}
static void HideChildWindows() {
    std::vector<DWORD> pids;
    if (g_serverPid) pids.push_back(g_serverPid);
    { std::lock_guard<std::mutex> lk(g_clientsLock); for (auto& c : g_clients) if (c.pid) pids.push_back((DWORD)c.pid); }
    if (!pids.empty()) EnumWindows(HideEnum_, (LPARAM)&pids);
}
static void ForceForeground(HWND hwnd) {
    HWND fg = GetForegroundWindow();
    DWORD myTid = GetCurrentThreadId();
    DWORD fgTid = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
    if (fgTid && fgTid != myTid) AttachThreadInput(myTid, fgTid, TRUE);
    BringWindowToTop(hwnd); SetForegroundWindow(hwnd); SetActiveWindow(hwnd); SetFocus(hwnd);
    if (fgTid && fgTid != myTid) AttachThreadInput(myTid, fgTid, FALSE);
}
// If a hidden child window grabbed the foreground (startup race), take it back so seat-0 input flows.
static void ReclaimForeground(HWND hwnd) {
    HWND fg = GetForegroundWindow();
    if (fg == hwnd) return;
    DWORD fgPid = 0; if (fg) GetWindowThreadProcessId(fg, &fgPid);
    bool childFg = (fgPid && fgPid == g_serverPid);
    if (!childFg) { std::lock_guard<std::mutex> lk(g_clientsLock); for (auto& c : g_clients) if (c.pid == fgPid) { childFg = true; break; } }
    if (childFg) ForceForeground(hwnd);
}
static void ClipCursorToWindow(HWND hwnd, bool on) {
    if (!on) { ClipCursor(NULL); return; }
    RECT cr; GetClientRect(hwnd, &cr);
    POINT a = { cr.left, cr.top }, b = { cr.right, cr.bottom };
    ClientToScreen(hwnd, &a); ClientToScreen(hwnd, &b);
    RECT clip = { a.x, a.y, b.x, b.y };
    ClipCursor(&clip);
}
#else
static void SaveGamma() {}
static void RestoreGamma() {}
static void HideChildWindows() {}
static void ReclaimForeground(void*) {}
static void ClipCursorToWindow(void*, bool) {}
#endif

// --- manager step: keyboard hold-to-join, pad reconnect, and the controller join state machine -----
// Runs each driver tick on the GUI thread (SDL pad state is refreshed there). The pure ss_join state
// machine decides transitions; this owns the side effects (enqueue spawn, write profile cfg, recall).
static void EnqueueSpawn(int pad, const char* profile);
static void ManagerStep() {
    uint32_t now = SDL_GetTicks();
    static uint32_t joinPrev[4] = { ss::kPadUnsampled, ss::kPadUnsampled, ss::kPadUnsampled, ss::kPadUnsampled };
    static uint32_t bannerUntil = 0;
    static ss::JoinState js;
    static uint32_t kbHoldStart = 0;
    static int   stickDir = 0; static uint32_t stickNext = 0;
    static bool  holdArmed = false; static uint32_t holdStart = 0;
    static int   lastW1 = -1, lastW2 = -1; static bool known = false;
    static int   prevMode = ss::JM_EDIT;

    // Keyboard hold-to-join: hold Enter (no kbd/mouse seat + room) for kJoinHoldMs to bring seat 0 up.
    {
        bool canKbm = !Seat0Alive() && LiveCountLocked() < 4;
        int kbHoldMs = 0;
        if (g_enterDown && canKbm) { if (!kbHoldStart) kbHoldStart = now; kbHoldMs = (int)(now - kbHoldStart); }
        else kbHoldStart = 0;
        g_kbHold = canKbm ? ss::JoinHoldPermille(kbHoldMs) : 0;
        if (kbHoldMs >= ss::kJoinHoldMs) { kbHoldStart = 0; EnqueueSpawn(-1, g_cfg.kbmProfile[0] ? g_cfg.kbmProfile : nullptr); }
    }

    // Reconnect: a controller that came back (possibly on a new slot) rebinds to the seat whose pad went
    // away, so unplug/replug resumes that seat instead of stranding the player.
    int waitingSeat = -1;
    {
        std::lock_guard<std::mutex> lk(g_clientsLock);
        for (size_t i = 0; i < g_clients.size(); ++i)
            if (g_clients[i].alive && g_clients[i].pad >= 0 && g_clients[i].pad < 4 && !PadConnected(g_clients[i].pad)) { waitingSeat = (int)i; break; }
    }
    if (waitingSeat >= 0) {
        int freePad = -1;
        for (int p = 0; p < 4; ++p) if (PadConnected(p) && !PadAssigned(p)) { freePad = p; break; }
        if (freePad >= 0) { std::lock_guard<std::mutex> lk(g_clientsLock); g_clients[waitingSeat].pad = freePad; }
    }

    // Controller create/join flow. START opens the panel; dpad OR left stick navigate; hold START commits;
    // B backs out; unplug or a 20s idle aborts. core/ss_join (tested) owns the transitions.
    bool taken = false;
    int  joinHoldPm = 0;
    if (js.pad < 0) {
        stickDir = 0; lastW1 = -1; lastW2 = -1; known = false;
        holdArmed = false; holdStart = 0;
        for (int p = 0; p < 4; ++p) {                    // idle: scan free pads for a fresh START press
            if (!PadConnected(p) || PadAssigned(p)) { joinPrev[p] = ss::kPadUnsampled; continue; }
            unsigned b = (unsigned)SdlXInputMask(g_pads[p]), prev = joinPrev[p]; joinPrev[p] = b;
            bool start = ss::PadButtonPressed(prev, b, 0x0010);   // XINPUT_GAMEPAD_START
            if (ss::JoinTryStart(js, p, start, LiveCountLocked() < 4)) {
                bannerUntil = now + 20000;
                LoadSeatLast(NextControllerSlot(), js);
                EnumerateProfiles(g_profiles);
                break;
            }
        }
    } else {
        bool ok = (js.pad >= 0 && js.pad < 4) && PadConnected(js.pad);
        SDL_GameController* gc = ok ? g_pads[js.pad] : nullptr;
        unsigned b = gc ? (unsigned)SdlXInputMask(gc) : 0, prev = joinPrev[js.pad]; joinPrev[js.pad] = b;
        unsigned nb = b & ~prev;                         // newly pressed this frame
        int sx = gc ? SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX) : 0;
        int sy = gc ? -SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY) : 0;   // +up
        int ax = sx < 0 ? -sx : sx, ay = sy < 0 ? -sy : sy;
        const int ON = 18000, OFF = 11000;
        int dir = 0;
        if (ay >= ax) { if (sy > ON) dir = 1; else if (sy < -ON) dir = 2; }
        else          { if (sx > ON) dir = 4; else if (sx < -ON) dir = 3; }
        if (dir == 0 && ax < OFF && ay < OFF) stickDir = 0;
        int stick = 0;
        if (dir != 0) {
            if (dir != stickDir) { stick = dir; stickDir = dir; stickNext = now + 380; }
            else if (now >= stickNext) { stick = dir; stickNext = now + 130; }
        }
        bool startHeld = ok && (b & 0x0010) != 0;
        if (!startHeld) holdArmed = true;
        int holdMs = 0;
        if (holdArmed && startHeld) { if (!holdStart) holdStart = now; holdMs = (int)(now - holdStart); }
        else holdStart = 0;
        if (holdMs > 0) bannerUntil = now + 20000;
        joinHoldPm = ss::JoinHoldPermille(holdMs);
        if (nb || stick) bannerUntil = now + 20000;
        ss::JoinInput in;
        in.newButtons = ss::JoinButtonsFromXInput((int)nb)
                      | (stick == 3 ? ss::JB_LEFT  : 0) | (stick == 4 ? ss::JB_RIGHT : 0)
                      | (stick == 1 ? ss::JB_UP    : 0) | (stick == 2 ? ss::JB_DOWN  : 0);
        in.confirmHoldMs  = (js.mode != ss::JM_VARIANT) ? holdMs : 0;
        in.connected      = ok;
        in.timedOut       = (now > bannerUntil);
        in.word1Count     = ss::ProfileWord1Count();
        in.word2Count     = ss::ProfileWord2Count();
        in.crosshairCount = ss::kCrosshairMax + 1;
        in.profileCount   = g_profiles.count;
        in.selTaken       = (js.mode == ss::JM_BROWSE && js.browseIndex >= 0 && js.browseIndex < g_profiles.count)
                            ? NameInUse(g_profiles.names[js.browseIndex]) : false;
        char curName[64]; ss::ProfileComposeName(curName, sizeof(curName), js.word1, js.word2);
        in.nameInUse = NameInUse(curName);
        taken = in.nameInUse;
        ss::JoinResult r = ss::JoinAdvance(js, in);
        if (r.action == ss::JoinAction::Join) {
            int slot = NextControllerSlot();
            char base[64], nm[64]; int ch = r.crosshair, mo = r.motion;
            if (r.fromBrowse && r.browseIndex >= 0 && r.browseIndex < g_profiles.count) {
                snprintf(base, sizeof(base), "%s", g_profiles.names[r.browseIndex]);
                LoadProfileCfg(base, &ch, &mo);
            } else {
                ss::ProfileComposeName(base, sizeof(base), r.word1, r.word2);
            }
            if (r.variant)         { VariantNameFor(nm, sizeof(nm), base); WriteProfileCfg(nm, ch, mo); }
            else if (r.fromBrowse) { snprintf(nm, sizeof(nm), "%s", base); }
            else                   { snprintf(nm, sizeof(nm), "%s", base); WriteProfileCfg(nm, ch, mo); }
            EnqueueSpawn(r.pad, nm);
            if (!r.fromBrowse) SaveSeatLast(slot, r.word1, r.word2, r.crosshair, r.motion);
        }
        if (js.mode == ss::JM_BROWSE && prevMode != ss::JM_BROWSE) EnumerateProfiles(g_profiles);
        if (js.mode == ss::JM_VARIANT) { holdArmed = false; holdStart = 0; }
        prevMode = js.mode;
        if (js.pad >= 0 && (js.word1 != lastW1 || js.word2 != lastW2)) {   // returning-player settings reload
            lastW1 = js.word1; lastW2 = js.word2;
            char ln[64]; ss::ProfileComposeName(ln, sizeof(ln), js.word1, js.word2);
            int lc, lm; known = LoadProfileCfg(ln, &lc, &lm);
            if (known) { js.crosshair = lc; js.motion = lm; }
        }
        if (js.mode == ss::JM_VARIANT) VariantNameFor(g_joinVariant, sizeof(g_joinVariant), curName);
        else g_joinVariant[0] = '\0';
    }
    // publish for the overlay (GUI thread)
    g_joinPad = js.pad; g_joinField = js.field; g_joinW1 = js.word1; g_joinW2 = js.word2;
    g_joinCross = js.crosshair; g_joinMotion = js.motion; g_joinTaken = taken; g_joinKnown = known;
    g_joinHold = joinHoldPm; g_joinMode = js.mode; g_browseIndex = js.browseIndex;
}

// --- the Qt Quick compositor: one item per seat -------------------------------------------------
class SeatItem : public QQuickItem {
    Q_OBJECT
public:
    explicit SeatItem(QQuickItem* parent = nullptr) : QQuickItem(parent) { setFlag(ItemHasContents, true); }
    void setSeat(int s) { m_seat = s; }
protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override {
        int fw = 0, fh = 0;
        QSGTexture* tex = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_clientsLock);
            if (m_seat >= 0 && m_seat < (int)g_clients.size() && g_clients[m_seat].alive) {
                Client& c = g_clients[m_seat];
                OpenFb(c);
                const int32_t* hdr = c.view ? (const int32_t*)c.view : nullptr;
                if (hdr && hdr[0] == FB_MAGIC && hdr[1] > 0 && hdr[2] > 0) {
                    fw = hdr[1]; fh = hdr[2];
                    QImage img(c.view + FB_PIXELS_OFF, fw, fh, fw * 4, QImage::Format_RGB32);   // BGRA bytes
                    tex = window()->createTextureFromImage(img);   // copies before we release the lock
                }
            }
        }
        if (!tex) { delete old; return nullptr; }
        QSGImageNode* node = static_cast<QSGImageNode*>(old);
        if (!node) node = window()->createImageNode();
        node->setTexture(tex);
        node->setOwnsTexture(true);
        node->setTextureCoordinatesTransform(QSGImageNode::MirrorVertically);   // glReadPixels is bottom-up
        ss::Rect b = ss::Letterbox(ss::Rect{ 0, 0, (int)width(), (int)height() }, fw, fh);
        node->setRect(QRectF(b.x, b.y, b.w, b.h));
        return node;
    }
private:
    int m_seat = -1;
};

// --- seat-0 keyboard/mouse routing (Qt events) --------------------------------------------------
// Extended keys carry the DIK high bit (0x80); the engine expects DirectInput scancodes. (Windows
// nativeScanCode covers the rest; mac scancode semantics need validation alongside the mac capture hook.)
static int QtKeyToDikExt(int key) {
    switch (key) {
        case Qt::Key_Up: return 0xC8; case Qt::Key_Down: return 0xD0;
        case Qt::Key_Left: return 0xCB; case Qt::Key_Right: return 0xCD;
        case Qt::Key_Home: return 0xC7; case Qt::Key_End: return 0xCF;
        case Qt::Key_PageUp: return 0xC9; case Qt::Key_PageDown: return 0xD1;
        case Qt::Key_Insert: return 0xD2; case Qt::Key_Delete: return 0xD3;
        default: return 0;
    }
}
static int QtKeyToGui(int key) {
    switch (key) {
        case Qt::Key_Escape: return 27; case Qt::Key_Return: case Qt::Key_Enter: return 13; case Qt::Key_Tab: return 9;
        case Qt::Key_Backspace: return 8; case Qt::Key_Delete: return 26;
        case Qt::Key_Up: return 11; case Qt::Key_Down: return 10; case Qt::Key_Left: return 5; case Qt::Key_Right: return 6;
        case Qt::Key_PageUp: return 2; case Qt::Key_PageDown: return 1; case Qt::Key_Home: return 3; case Qt::Key_End: return 4;
        default: return 0;
    }
}
// Send a key/button UP to seat 0 (the kbd/mouse player). Used to auto-release stuck keys.
static void Seat0KeyUp(int sc) {
    std::lock_guard<std::mutex> lk(g_clientsLock);
    if (!g_clients.empty() && g_clients[0].alive && g_clients[0].pad < 0) PushKey(g_clients[0], sc, 0, 0, 0);
}

// App-level filter: routes keyboard + mouse buttons to seat 0 (the kbd/mouse player), and tracks Enter
// for keyboard hold-to-join. Look (relative mouse) is handled by the driver's cursor re-center. Every key
// it sends DOWN is recorded in g_heldVk so the driver can auto-release it if its physical key goes up
// without a matching Qt release (alt-tab, focus loss) -- which is what caused the stuck-strafe bug.
class InputFilter : public QObject {
    Q_OBJECT
protected:
    bool eventFilter(QObject* o, QEvent* e) override {
        switch (e->type()) {
            case QEvent::KeyPress: case QEvent::KeyRelease: {
                QKeyEvent* k = static_cast<QKeyEvent*>(e);
                bool down = (e->type() == QEvent::KeyPress);
                if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) g_enterDown = down;
                if (k->isAutoRepeat()) break;
                int sc = QtKeyToDikExt(k->key()); if (!sc) sc = (int)(k->nativeScanCode() & 0xFF);
                int gk = QtKeyToGui(k->key());
                if (sc) g_seat0Down[sc & 0x1FF] = down ? 1 : 0;   // track for stuck-key auto-release
                std::lock_guard<std::mutex> lk(g_clientsLock);
                if (!g_clients.empty() && g_clients[0].alive && g_clients[0].pad < 0) {
                    if (sc || gk) PushKey(g_clients[0], sc, gk, 0, down ? 1 : 0);
                    if (down) { QByteArray t = k->text().toLatin1(); for (char ch : t) if ((unsigned char)ch >= ' ') PushKey(g_clients[0], 0, 0, (unsigned char)ch, 1); }
                }
                break;
            }
            case QEvent::MouseButtonPress: case QEvent::MouseButtonRelease: {
                if (g_mouseFreed) break;
                QMouseEvent* m = static_cast<QMouseEvent*>(e);
                bool down = (e->type() == QEvent::MouseButtonPress);
                int code = (m->button() == Qt::LeftButton) ? 0x100 : (m->button() == Qt::RightButton) ? 0x101 : (m->button() == Qt::MiddleButton) ? 0x102 : 0;
                if (code) {
                    g_seat0Down[code & 0x1FF] = down ? 1 : 0;   // tracked so focus-loss releases it too
                    std::lock_guard<std::mutex> lk(g_clientsLock);
                    if (!g_clients.empty() && g_clients[0].alive && g_clients[0].pad < 0)
                        PushKey(g_clients[0], code, 0, 0, down ? 1 : 0);
                }
                break;
            }
            default: break;
        }
        (void)o;
        return false;   // never consume -- let Qt handle the window normally too
    }
};

// --- the QML overlay (the existing qml/Overlay.qml, loaded straight into the scene) ----------------
// No D3D11/Metal interop: the overlay is just another item in the same Qt Quick scene, drawn above the
// seats (z=100). Its root properties are pushed each tick exactly as the old overlay_qt.cpp did.
static QQuickItem* g_overlay = nullptr;
static QQmlEngine* g_qmlEngine = nullptr;

static QString FindOverlayQml() {
    QString dir = QCoreApplication::applicationDirPath();
    QStringList cands;
    if (const char* env = getenv("SS_OVERLAY_QML")) cands << QString::fromUtf8(env);
    cands << dir + "/../overlay/qml/Overlay.qml"        // dev / build layout (host in build/)
          << dir + "/overlay/qml/Overlay.qml"           // packaged layout (overlay beside the binary)
          << dir + "/../Resources/overlay/qml/Overlay.qml";  // macOS .app bundle layout
    for (const QString& c : cands) if (QFileInfo::exists(c)) return c;
    return QString();
}
static bool InitOverlay(QQuickWindow& window) {
    QString path = FindOverlayQml();
    if (path.isEmpty()) { qWarning("overlay: Overlay.qml not found (set SS_OVERLAY_QML)"); return false; }
    g_qmlEngine = new QQmlEngine();
    QQmlComponent comp(g_qmlEngine, QUrl::fromLocalFile(path));
    QObject* obj = comp.create();
    if (!obj) { qWarning("overlay QML error: %s", comp.errorString().toUtf8().constData()); return false; }
    g_overlay = qobject_cast<QQuickItem*>(obj);
    if (!g_overlay) { delete obj; return false; }
    g_overlay->setParentItem(window.contentItem());
    g_overlay->setZ(100);   // always above the seat items
    return true;
}
// Push the host's published state onto the overlay's QML root properties (GUI thread).
static void UpdateOverlay(int W, int H, const ss::Rect& joinArea, int joinSeatPane, const QVariantList& disconnects) {
    if (!g_overlay) return;
    g_overlay->setWidth(W); g_overlay->setHeight(H);
    g_overlay->setProperty("screenW", W);
    g_overlay->setProperty("screenH", H);
    g_overlay->setProperty("seat0Gone", g_kbmEverUp && !Seat0Alive());
    g_overlay->setProperty("waitStart", !g_autoStart && g_launchN == 0);
    g_overlay->setProperty("kbHold", g_kbHold / 1000.0);
    g_overlay->setProperty("disconnects", disconnects);

    if (g_joinPad >= 0) {
        QVariantMap pane{ {"x", joinArea.x}, {"y", joinArea.y}, {"w", joinArea.w}, {"h", joinArea.h} };
        g_overlay->setProperty("join", QVariantMap{
            {"controller", g_joinPad + 1}, {"seat", joinSeatPane}, {"field", g_joinField},
            {"word1", QString::fromUtf8(ss::ProfileWord1(g_joinW1))}, {"word2", QString::fromUtf8(ss::ProfileWord2(g_joinW2))},
            {"crosshair", g_joinCross}, {"motion", g_joinMotion != 0}, {"taken", g_joinTaken != 0}, {"known", g_joinKnown != 0},
            {"hold", g_joinHold / 1000.0}, {"mode", g_joinMode}, {"variant", QString::fromUtf8(g_joinVariant)},
            {"pane", pane} });
    } else {
        g_overlay->setProperty("join", QVariant());
    }

    if (g_joinPad >= 0 && g_joinMode == ss::JM_BROWSE) {
        QVariantList items;
        for (int i = 0; i < g_profiles.count; ++i)
            items.append(QVariantMap{ {"name", QString::fromUtf8(g_profiles.names[i])}, {"taken", NameInUse(g_profiles.names[i])} });
        g_overlay->setProperty("browse", QVariantMap{ {"index", g_browseIndex}, {"items", items} });
    } else {
        g_overlay->setProperty("browse", QVariant());
    }
}

static void ParseEnv() {
    g_invMouseY = getenv("SS_MOUSE_INVY") != nullptr;
    g_invPadY   = getenv("SS_PAD_INVY")   != nullptr;
    g_invPadX   = getenv("SS_PAD_INVX")   != nullptr;
    if (const char* as = getenv("SS_AUTOSTART"); as && as[0] == '0') g_autoStart = false;
    if (const char* kp = getenv("SS_KBM_PROFILE"); kp && kp[0])
        snprintf(g_cfg.kbmProfile, sizeof(g_cfg.kbmProfile), "%s", kp);
}

int main(int argc, char** argv) {
    if (argc < 5) {
        printf("usage: host <numPlayers> <hook-lib> <zandronum> <iwad> [gamedir] [preferred.cfg] [absolute.cfg]\n");
        return 1;
    }
    g_cfg.numPlayers  = atoi(argv[1]); if (g_cfg.numPlayers < 1) g_cfg.numPlayers = 1; if (g_cfg.numPlayers > 4) g_cfg.numPlayers = 4;
    g_cfg.hookLib     = argv[2];
    g_cfg.engineExe   = argv[3];
    g_cfg.iwad        = argv[4];
    g_cfg.gamedir     = (argc > 5) ? argv[5] : ".";
    g_cfg.seatsCfg    = (argc > 6) ? argv[6] : nullptr;
    g_cfg.absoluteCfg = (argc > 7) ? argv[7] : nullptr;
    ParseEnv();

#ifdef _WIN32
    // Hide our OWN console (play.ps1 launches us detached) so only the game window shows -- but only when
    // we own the console, never a shell we were launched into interactively.
    { DWORD cpl[2]; if (GetConsoleProcessList(cpl, 2) == 1) ShowWindow(GetConsoleWindow(), SW_HIDE); }
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    SDL_SetMainReady();
    SDL_Init(SDL_INIT_GAMECONTROLLER);

    QGuiApplication app(argc, argv);
    if (QScreen* sc = QGuiApplication::primaryScreen()) {
        qreal dpr = sc->devicePixelRatio();
        g_screenW = (int)(sc->geometry().width()  * dpr);
        g_screenH = (int)(sc->geometry().height() * dpr);
    }

    SaveGamma();        // snapshot the desktop gamma BEFORE any client can change it
    atexit(RestoreGamma);

    DeploySeatCfg();    // write splitseat.cfg + splitseat_absolute.cfg into the gamedir before any client

    std::thread worker(WorkerMain);   // server bring-up + spawns run here (off the GUI loop)

    InputFilter inputFilter;
    app.installEventFilter(&inputFilter);   // seat-0 keyboard/mouse + Enter hold-to-join

    QQuickWindow window;
    window.setTitle("splitdronum");
    window.setColor(QColor(0, 0, 0));
    window.resize(1280, 720);
    window.setCursor(Qt::BlankCursor);
    QQuickItem* root = window.contentItem();
    SeatItem* seats[4];
    for (int i = 0; i < 4; ++i) { seats[i] = new SeatItem(root); seats[i]->setSeat(i); seats[i]->setVisible(false); }
    InitOverlay(window);   // load qml/Overlay.qml on top of the seats (optional -- host runs without it)
    window.show();
    window.requestActivate();
#ifdef _WIN32
    HWND winHandle = (HWND)window.winId();
    // play.ps1 launches us with -WindowStyle Hidden (to hide the console); that STARTUPINFO SW_HIDE would
    // otherwise suppress our first window-show, so force the window visible, then claim foreground.
    ShowWindow(winHandle, SW_SHOWNORMAL);
    ForceForeground(winHandle);
#else
    void* winHandle = nullptr;
#endif
    bool lookPrimed = false;   // skip the first cursor delta after (re)gaining capture (avoids a jump)
    bool wasActive  = true;    // track focus transitions to release seat-0 keys on focus loss

    // ~60 Hz driver (GUI thread): poll pads, feed gameplay, run the manager step (join/keyboard), reap,
    // music handoff, relative-mouse look, then re-tile + repaint.
    QTimer driver;
    QObject::connect(&driver, &QTimer::timeout, [&]() {
        SDL_GameControllerUpdate();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if      (e.type == SDL_CONTROLLERDEVICEADDED)   OnControllerAdded(e.cdevice.which);
            else if (e.type == SDL_CONTROLLERDEVICEREMOVED) OnControllerRemoved(e.cdevice.which);
        }
        FeedGameplay();
        ManagerStep();
        ReapDeadSeats();
        UpdateMusicOwner();
        HideChildWindows();            // keep the stock clients' own windows hidden (Win)
        ReclaimForeground(winHandle);  // take focus back from a child that grabbed it (Win)

        // free the mouse when seat 0 is in a menu/console, or when every live seat is (nobody playing);
        // otherwise capture it for seat-0 look by re-centering the cursor and feeding the delta.
        {
            std::lock_guard<std::mutex> lk(g_clientsLock);
            bool seat0Menu = !g_clients.empty() && g_clients[0].in && g_clients[0].in[IN_MENU] != 0;
            bool allMenu = true;
            for (auto& c : g_clients) if (c.alive && !(c.in && c.in[IN_MENU] != 0)) { allMenu = false; break; }
            g_mouseFreed = seat0Menu || allMenu;
        }
        bool active = window.isActive();
        // Auto-release stuck seat-0 modifiers (cross-platform): ask the OS for the real modifier state and
        // release any modifier the host still holds that is no longer physically down. This self-heals the
        // stuck-strafe bug (alt-tab / cmd-tab leaves Alt=+strafe held). On focus loss, release everything
        // held (covers non-modifier keys lost across the switch). core/ss_input decides; it's unit-tested.
        {
            Qt::KeyboardModifiers mods = QGuiApplication::queryKeyboardModifiers();
            int rel[8];
            int n = ss::StuckModifierReleases(g_seat0Down, 0x200,
                        (mods & Qt::AltModifier) != 0, (mods & Qt::ControlModifier) != 0, (mods & Qt::ShiftModifier) != 0,
                        rel, 8);
            for (int i = 0; i < n; ++i) { g_seat0Down[rel[i]] = 0; Seat0KeyUp(rel[i]); }
        }
        if (wasActive && !active)
            for (int sc = 1; sc < 0x200; ++sc) if (g_seat0Down[sc]) { g_seat0Down[sc] = 0; Seat0KeyUp(sc); }
        wasActive = active;

        bool capture = active && !g_mouseFreed && Seat0Alive();
        ClipCursorToWindow(winHandle, capture);
        if (capture) {
            QPoint center(window.x() + (int)window.width() / 2, window.y() + (int)window.height() / 2);
            if (!lookPrimed) { QCursor::setPos(center); lookPrimed = true; }   // arm without a jump
            else {
                QPoint cur = QCursor::pos();
                int dx = cur.x() - center.x(), dy = cur.y() - center.y();
                if (dx || dy) {
                    std::lock_guard<std::mutex> lk(g_clientsLock);
                    if (!g_clients.empty() && g_clients[0].alive && g_clients[0].pad < 0)
                        PushMouse(g_clients[0], dx, g_invMouseY ? dy : -dy);
                    QCursor::setPos(center);
                }
            }
        } else {
            lookPrimed = false;
        }

        int W = (int)window.width(), H = (int)window.height();
        ss::Rect paneFor[4]; bool show[4] = { false, false, false, false };
        ss::Rect joinArea{ 0, 0, W, H }; int joinSeatPane = 0;
        QVariantList disconnects;
        {
            std::lock_guard<std::mutex> lk(g_clientsLock);
            int live = LiveCount();
            // a controller customizing reserves its OWN pane (live+pending) so its card doesn't overlap seats
            int pending = (g_joinPad >= 0 && live < 4) ? 1 : 0;
            ss::Layout lay = ss::ComputeLayout(live + pending, W, H, ss::TwoMode::Auto);
            int pane = 0;
            for (int i = 0; i < 4; ++i) {
                bool alive = i < (int)g_clients.size() && g_clients[i].alive;
                if (alive && pane < lay.count) {
                    paneFor[i] = lay.panes[pane]; show[i] = true;
                    if (g_clients[i].pad >= 0 && g_clients[i].pad < 4 && !PadConnected(g_clients[i].pad)) {
                        ss::Rect pn = lay.panes[pane];
                        disconnects.append(QVariantMap{ {"seat", pane},
                            {"pane", QVariantMap{ {"x", pn.x}, {"y", pn.y}, {"w", pn.w}, {"h", pn.h} }} });
                    }
                    pane++;
                }
            }
            joinSeatPane = pane;
            joinArea = (pending && pane < lay.count) ? lay.panes[pane] : ss::Rect{ 0, 0, W, H };
        }
        for (int i = 0; i < 4; ++i) {
            seats[i]->setVisible(show[i]);
            if (show[i]) {
                seats[i]->setPosition(QPointF(paneFor[i].x, paneFor[i].y));
                seats[i]->setSize(QSizeF(paneFor[i].w, paneFor[i].h));
                seats[i]->update();
            }
        }
        UpdateOverlay(W, H, joinArea, joinSeatPane, disconnects);
    });
    driver.start(16);

    int rc = app.exec();

    // shutdown: stop the worker, quit all clients cleanly (saves their inis), release shared memory + pads.
    { std::lock_guard<std::mutex> lk(g_actMx); g_quit = true; } g_actCv.notify_one();
    if (worker.joinable()) worker.join();
    {
        std::lock_guard<std::mutex> lk(g_clientsLock);
        for (auto& c : g_clients) if (c.pid) ss::ProcKillGraceful(c.pid);
        for (auto& c : g_clients) UnmapSeat(c);
    }
    if (g_serverPid) ss::ProcKillGraceful(g_serverPid);
    ClipCursorToWindow(winHandle, false);   // release the cursor
    RestoreGamma();                          // put the desktop gamma back (engine clobbers it)
    for (int p = 0; p < 4; ++p) if (g_pads[p]) SDL_GameControllerClose(g_pads[p]);
    SDL_Quit();
#ifdef _WIN32
    WSACleanup();
#endif
    return rc;
}

#include "host_qt.moc"   // AUTOMOC: SeatItem's Q_OBJECT lives in this .cpp
