// host -- the splitscreen compositor (Phase C).
//
// Launches N stock Zandronum clients with ss_hook.dll injected, reads each client's framebuffer
// from its "ZanDLLFB_<pid>" shared section, and composites them into ONE window using core/.
// The clients render normally (the DLL captures their GL back buffer); their own windows are
// hidden so only this compositor is visible.
//
// This first cut composites N independent (solo) clients to prove the window + layout + blit
// path. Co-op (a loopback server the clients join) and input routing come next.
//
//   host.exe <numPlayers> <full\ss_hook.dll> <full\zandronum.exe> <iwad> [gamedir]
#include <winsock2.h>          // free-port probe (must precede windows.h to avoid the winsock1 clash)
#include <ws2tcpip.h>
#include <windows.h>
#include <timeapi.h>           // timeBeginPeriod -- 1ms timer so the present loop isn't ~64fps
#include <dwmapi.h>            // DwmFlush -- GDI fallback path only
#include <d3d11.h>             // GPU compositor: textured quads -> swap chain (GDI couldn't push a
#include <d3dcompiler.h>       // large window at high refresh; the GPU does it trivially)
#define XINPUT_USE_9_1_0
#include <xinput.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include "../core/ss_layout.h"
#include "../core/ss_join.h"    // pure controller drop-in state machine (100%-tested; host owns the glue)
#include "../core/ss_profile.h" // pure profile list + client launch-arg builder (100%-tested)

// --- inject the DLL into a freshly-launched stock client, in two phases so the engine's main thread
//     stays suspended until the DLL's window hooks are live (no startup-window flash). Splitting
//     inject from resume lets us inject ALL clients first, so their hook-installs run concurrently,
//     then resume them -- the per-client hook wait overlaps instead of serializing. ---
struct Launch { DWORD pid; HANDLE hProc, hThread, hReady; };
static Launch InjectSuspended(const char* dll, const char* exe, const char* cwd, const char* args) {
    Launch L = { 0, NULL, NULL, NULL };
    char cmd[8192];
    _snprintf(cmd, sizeof(cmd), "\"%s\" %s", exe, args);
    cmd[sizeof(cmd) - 1] = '\0';
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, cwd, &si, &pi))
        return L;
    char ev[64];
    _snprintf(ev, sizeof(ev), "ZanHooks_%lu", pi.dwProcessId);
    L.hReady = CreateEventA(NULL, TRUE, FALSE, ev);   // the DLL sets this once its window hooks are live
    SIZE_T len = strlen(dll) + 1;
    void* mem = VirtualAllocEx(pi.hProcess, NULL, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    WriteProcessMemory(pi.hProcess, mem, dll, len, NULL);
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    HANDLE th = CreateRemoteThread(pi.hProcess, NULL, 0,
        (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryA"), mem, 0, NULL);
    WaitForSingleObject(th, 10000);
    CloseHandle(th);
    VirtualFreeEx(pi.hProcess, mem, 0, MEM_RELEASE);
    L.pid = pi.dwProcessId; L.hProc = pi.hProcess; L.hThread = pi.hThread;
    return L;
}
static DWORD ResumeAfterHooks(Launch& L) {
    if (!L.hThread) return 0;
    if (L.hReady) { WaitForSingleObject(L.hReady, 5000); CloseHandle(L.hReady); }   // wait for the window hooks
    ResumeThread(L.hThread);
    CloseHandle(L.hThread); CloseHandle(L.hProc);
    return L.pid;
}

// --- a client's published framebuffer ("ZanDLLFB_<pid>": int[0]=magic w h frame, then BGR bottom-up) ---
static const int FB_MAGIC = 0x5A444C46;
static const int IN_MAGIC = 0x5A414E49;   // 'ZANI' -- the host->client input section (matches the DLL)
static const int KEY_RING = 64;
static const int IN_AXES    = 136;        // ZanIN int index of the 5 controller axes (value x1000)
static const int IN_MENU    = 160;        // ZanIN int the DLL writes back: 1 = that seat is in a menu/console
static const int IN_CMDSEQ  = 164;        // bumped after writing a console command at IN_CMDTEXT (DLL runs it)
static const int IN_CMDTEXT = 168;        // command string bytes, up to IN_CMDMAX
static const int IN_CMDMAX  = 120;
static volatile LONG g_mouseFreed = 0;    // 1 = release the mouse (a menu is up) so it can roam the desktop
static float g_renderScale = 1.0f;        // client render res = monitor size * this (SS_RENDER_SCALE; <1 = faster/softer)
static bool  g_smartScale  = true;        // scale render res down as more seats join (SS_SMARTSCALE=0 disables)
enum { AX_Yaw = 0, AX_Pitch = 1, AX_Forward = 2, AX_Side = 3 };   // m_joy.h order

// Optional per-axis look inversion (set from the environment by play.ps1). Defaults give
// mouse-up / stick-up = look up and stick-right = turn right; toggle any axis that feels wrong.
static bool g_invMouseY = false, g_invPadY = false, g_invPadX = false;
struct Client {
    DWORD pid = 0;
    HANDLE map = nullptr;
    const unsigned char* view = nullptr;
    HANDLE inMap = nullptr;            // ZanIN_<pid>: host -> client input
    volatile LONG* in = nullptr;
    int keyWrite = 0;
    bool alive = true;                 // dynamic drop-in: false once this seat has left (slot reusable)
    int  pad = -1;                     // XInput index driving this seat; -1 = seat 0 (keyboard+mouse)
    char name[48] = "";                // this seat's player/profile name -- for the no-duplicate-profile check
    HANDLE hproc = nullptr;            // SYNCHRONIZE handle to the client process -> detect quit-game/crash
};
static std::vector<Client> g_clients;
static CRITICAL_SECTION    g_clientsLock;   // guards g_clients mutation (manager) vs read (render/main)
// Create-and-join flow state, published by the manager thread for the render thread (DrawJoinPrompt):
static volatile LONG       g_joinPad = -1;  // controller currently in the create flow (its XInput index), else -1
static volatile LONG       g_joinStep = 0;  // ss::JoinStep: prompt / word1 / word2 / crosshair / motion-comp
static volatile LONG       g_joinW1 = 0, g_joinW2 = 0;          // name word indices being scrolled
static volatile LONG       g_joinCross = 0, g_joinMotion = 0;   // chosen crosshair value + motion-comp flag
static volatile LONG       g_joinTaken = 0;                     // 1 = composed name already loaded (create blocked)
static bool                g_padConn[4] = { false, false, false, false };  // XInput slot connected? (manager-updated)
static int g_port = 10666;           // loopback server/connect port; resolved to a free one at startup
static std::vector<DWORD>  g_hidePids;   // server + clients: their own windows are kept hidden
// live (alive) seat count -- callers already hold g_clientsLock
static int LiveCount() { int n = 0; for (auto& c : g_clients) if (c.alive) ++n; return n; }

// the staggered launch runs on a background thread so the compositor window is up immediately
struct LaunchArgs { const char* dll; const char* exe; const char* iwad; const char* cwd; int n; const char* seatsCfg; };
static LaunchArgs  g_launch;
static volatile LONG g_launchN = 0;      // clients spawned so far (for the loading text)
static HWND        g_hwnd = NULL;        // compositor window (the render thread composites into it)

// launch a process WITHOUT injecting (the dedicated server doesn't need the DLL)
static DWORD LaunchPlain(const char* exe, const char* cwd, const char* args) {
    char cmd[8192];
    _snprintf(cmd, sizeof(cmd), "\"%s\" %s", exe, args);
    cmd[sizeof(cmd) - 1] = '\0';
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, cwd, &si, &pi)) return 0;
    DWORD pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return pid;
}

static void OpenFb(Client& c) {
    if (c.view) return;
    char nm[64];
    _snprintf(nm, sizeof(nm), "ZanDLLFB_%lu", c.pid);
    nm[sizeof(nm) - 1] = '\0';
    c.map = OpenFileMappingA(FILE_MAP_READ, FALSE, nm);
    if (!c.map) return;
    c.view = (const unsigned char*)MapViewOfFile(c.map, FILE_MAP_READ, 0, 0, 0);
    if (!c.view) { CloseHandle(c.map); c.map = nullptr; }
}

// create the input channel "ZanIN_<pid>" the DLL reads. Layout matches the DLL:
//   [0]=magic [3]=mouse dx [4]=mouse dy [5]=key write-count; ring of 64 events at [8].
static void CreateSeatIn(Client& c) {
    char nm[64];
    _snprintf(nm, sizeof(nm), "ZanIN_%lu", c.pid);
    nm[sizeof(nm) - 1] = '\0';
    c.inMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 1024, nm);
    if (!c.inMap) return;
    c.in = (volatile LONG*)MapViewOfFile(c.inMap, FILE_MAP_WRITE, 0, 0, 1024);
    if (c.in) c.in[0] = IN_MAGIC;
}

// Robustly pull our window to the foreground. The host is launched hidden + grandchild-style, so
// Windows' focus-stealing prevention makes a plain SetForegroundWindow a no-op -- the host then never
// holds focus, and keyboard (WM_KEYDOWN) + raw mouse (WM_INPUT, registered without INPUTSINK) only
// arrive at the FOREGROUND window, so seat 0 gets zero input. Attaching to the current foreground
// thread's input state lifts that restriction so the activation actually takes. THIS is the focus bug.
static void ForceForeground(HWND hwnd) {
    HWND fg = GetForegroundWindow();
    DWORD myTid = GetCurrentThreadId();
    DWORD fgTid = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
    if (fgTid && fgTid != myTid) AttachThreadInput(myTid, fgTid, TRUE);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);
    if (fgTid && fgTid != myTid) AttachThreadInput(myTid, fgTid, FALSE);
}

// Held state of seat 0's keys/mouse-buttons by scancode, so we can release them on focus loss. PushKey
// only ever drives seat 0 (controllers go through PushButton), so tracking it here is sufficient.
static bool g_seat0Down[0x110] = { false };

// push one key event into a seat's ring (a = (scancode<<1)|down, b = gk | (char<<16))
static void PushKey(Client& c, int sc, int gk, int ch, int down) {
    if (!c.in) return;
    if (sc > 0 && sc < 0x110) g_seat0Down[sc] = (down != 0);   // track for the focus-loss release below
    int slot = c.keyWrite % KEY_RING;
    c.in[8 + slot * 2]     = (sc << 1) | (down & 1);
    c.in[8 + slot * 2 + 1] = (gk & 0xffff) | (ch << 16);
    ++c.keyWrite;
    c.in[5] = c.keyWrite;             // publish last (the DLL reads [5], then the entries)
}

// Release every key/button seat 0 currently holds. Called when the host loses focus: Alt+Tab forwards
// Alt-down + Tab-down to seat 0 while we're foreground, then focus leaves before the key-UPs arrive, so
// those keys would stick down (Tab=automap, etc.) and input feels dead even after tabbing back.
static void ReleaseSeat0Keys() {
    if (g_clients.empty()) return;
    for (int sc = 1; sc < 0x110; ++sc) if (g_seat0Down[sc])
        PushKey(g_clients[0], sc, 0, 0, 0);   // sends key-up (and clears g_seat0Down[sc])
}

// accumulate a relative mouse delta for a seat (the DLL consumes + zeroes each frame)
static void PushMouse(Client& c, int dx, int dy) {
    if (!c.in) return;
    InterlockedAdd(&c.in[3], dx);
    InterlockedAdd(&c.in[4], dy);
}

// post a controller button as a RAW EV_KeyDown/Up (ring entry with the raw bit set) so the
// engine routes it through its OWN controller binds, in game and in menus -- no hardcoding.
static void PushButton(Client& c, int code, int down) {
    if (!c.in) return;
    int slot = c.keyWrite % KEY_RING;
    c.in[8 + slot * 2]     = (code << 1) | (down & 1) | (1 << 16);   // raw bit (bit 16)
    c.in[8 + slot * 2 + 1] = 0;
    ++c.keyWrite;
    c.in[5] = c.keyWrite;
}

// Windows VK -> engine GK_* GUI code (glue; values mirror the engine's d_gui.h). 0 = no GK.
static int VkToGuiKey(int vk) {
    switch (vk) {
        case VK_ESCAPE: return 27; case VK_RETURN: return 13; case VK_TAB: return 9;
        case VK_BACK:   return 8;  case VK_DELETE: return 26;
        case VK_UP:     return 11; case VK_DOWN:   return 10; case VK_LEFT: return 5; case VK_RIGHT: return 6;
        case VK_PRIOR:  return 2;  case VK_NEXT:   return 1;  case VK_HOME: return 3; case VK_END:   return 4;
        default: return 0;
    }
}

// --- hide the clients' own windows so only the compositor shows (runs every frame to catch
//     windows as they are created during client startup) ---
static BOOL CALLBACK HideProc(HWND hwnd, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    for (DWORD p : g_hidePids)
        if (p == pid && IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
    return TRUE;
}

// normalized stick value (-1..1), 0 inside the deadzone
static float StickAxis(short v, short dz) {
    if (v >  dz) return (float)(v - dz) / (32767 - dz);
    if (v < -dz) return (float)(v + dz) / (32767 - dz);
    return 0.f;
}
static void WriteAxis(Client& c, int idx, float v) {
    if (c.in) c.in[IN_AXES + idx] = (LONG)(v * 1000.0f);
}

// Send a console command to a running seat via the command channel: write the string, then bump the
// sequence (a barrier) so the DLL runs it exactly once. Used to move music ownership between seats.
static void WriteCommand(Client& c, const char* cmd) {
    if (!c.in) return;
    char* dst = (char*)(c.in + IN_CMDTEXT);
    _snprintf(dst, IN_CMDMAX, "%s", cmd);
    dst[IN_CMDMAX - 1] = '\0';
    InterlockedIncrement(&c.in[IN_CMDSEQ]);   // publish AFTER the text is in place
}

// --- XInput pads drive seats 1..N-1 (controller index = seat-1). We route each pad's analog
//     sticks into the engine's OWN joystick axes (the DLL's I_GetAxes hook), so G_BuildTiccmd
//     does NATIVE analog movement + joystick look -- no keyboard/mouse emulation. Only buttons
//     stay edge-detected key events. Clients run use_joystick 0 so the shared physical pad
//     doesn't leak into every seat. ---
struct PadState { WORD prevBtn; bool lt, rt; };
static PadState g_pad[4];

static int  LiveCountLocked() { EnterCriticalSection(&g_clientsLock); int n = LiveCount(); LeaveCriticalSection(&g_clientsLock); return n; }
static bool PadAssigned(int pad) {
    EnterCriticalSection(&g_clientsLock);
    bool a = false;
    for (auto& c : g_clients) if (c.alive && c.pad == pad) { a = true; break; }
    LeaveCriticalSection(&g_clientsLock);
    return a;
}
// Reap a dropped/left client GRACEFULLY: post WM_CLOSE to its (hidden) window so the engine runs its
// normal netgame quit -- which DISCONNECTS from the server -- then exits, so the server frees that
// player slot instead of leaving them lingering until a timeout. Force-kill only if it hangs.
static BOOL CALLBACK CloseByPid(HWND h, LPARAM pid) {
    DWORD wp = 0; GetWindowThreadProcessId(h, &wp);
    if (wp == (DWORD)pid) PostMessageA(h, WM_CLOSE, 0, 0);
    return TRUE;
}
static DWORD WINAPI ReapThread(LPVOID arg) {
    DWORD pid = (DWORD)(SIZE_T)arg;
    EnumWindows(CloseByPid, (LPARAM)pid);      // disconnect + quit cleanly
    HANDLE p = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (p) {
        if (WaitForSingleObject(p, 4000) != WAIT_OBJECT_0) TerminateProcess(p, 0);   // hung -> force-stop
        CloseHandle(p);
    }
    return 0;
}
// Release our shared-memory views + the process handle for this seat (no process action).
static void UnmapSeat(Client& c) {
    if (c.view)  { UnmapViewOfFile(c.view); c.view = nullptr; }
    if (c.map)   { CloseHandle(c.map);    c.map = nullptr; }
    if (c.in)    { UnmapViewOfFile((LPCVOID)c.in); c.in = nullptr; }
    if (c.inMap) { CloseHandle(c.inMap);  c.inMap = nullptr; }
    if (c.hproc) { CloseHandle(c.hproc);  c.hproc = nullptr; }
}
// Tear down one seat we are DROPPING (still running): unmap, then reap its process gracefully. Caller
// holds g_clientsLock; the slow disconnect+wait runs on a detached thread so the compositor never stalls.
static void CloseClient(Client& c) {
    UnmapSeat(c);
    if (c.pid) {
        HANDLE t = CreateThread(NULL, 0, ReapThread, (LPVOID)(SIZE_T)c.pid, 0, NULL);
        if (t) CloseHandle(t);
        else { HANDLE p = OpenProcess(PROCESS_TERMINATE, FALSE, c.pid); if (p) { TerminateProcess(p, 0); CloseHandle(p); } }
        c.pid = 0;
    }
}
// The seat's process EXITED on its own (in-game "quit game" or a crash): just release our handles and free
// the slot for reuse. No reap thread -- there's nothing left to quit and its pid may already be recycled.
static void FreeDeadSeat(Client& c) {
    UnmapSeat(c);
    c.pid = 0;
    c.alive = false;
}

// Seat 0 (keyboard/mouse) is pinned to slot 0, so this is the live-status of the kbd/mouse player.
static bool Seat0Alive() { return !g_clients.empty() && g_clients[0].alive && g_clients[0].pad < 0; }

// Free any seat whose process exited on its own -- in-game "quit game" or a crash -- so its slot/pad
// frees and the dead pane disappears. Runs on the manager thread each tick.
static void ReapDeadSeats() {
    EnterCriticalSection(&g_clientsLock);
    for (auto& c : g_clients)
        if (c.alive && c.hproc && WaitForSingleObject(c.hproc, 0) == WAIT_OBJECT_0) FreeDeadSeat(c);
    LeaveCriticalSection(&g_clientsLock);
}

// Exactly one seat plays music. Seat 0 (the music owner) plays it natively while alive; once it quits or
// crashes, hand music to the lowest-index live controller seat via the command channel, and take it back
// when seat 0 rejoins. Only emits a command when ownership actually changes. Runs on the manager thread.
static int g_musicOwner = -1;   // slot index of the controller currently told to play music, else -1
static void UpdateMusicOwner() {
    EnterCriticalSection(&g_clientsLock);
    int desired = -1;
    bool seat0 = !g_clients.empty() && g_clients[0].alive && g_clients[0].pad < 0;
    if (!seat0)
        for (size_t i = 1; i < g_clients.size(); ++i) if (g_clients[i].alive) { desired = (int)i; break; }
    if (desired != g_musicOwner) {
        if (g_musicOwner >= 1 && g_musicOwner < (int)g_clients.size() && g_clients[g_musicOwner].alive)
            WriteCommand(g_clients[g_musicOwner], "snd_musicvolume 0");           // mute the previous owner
        if (desired >= 0) WriteCommand(g_clients[desired], "exec splitseat.cfg"); // re-apply global cfg -> music on
        g_musicOwner = desired;
    }
    LeaveCriticalSection(&g_clientsLock);
}

// Drive every controller seat from its dynamically-assigned pad (c.pad): native analog axes + edge-
// detected buttons, exactly as before but keyed by pad rather than seat-1. Drop-out: hold BACK ~1.2s
// to leave (the seat despawns and its pad frees). Runs on the manager thread.
static DWORD g_backHold[4] = { 0 };
static void FeedGameplay() {
    EnterCriticalSection(&g_clientsLock);
    for (auto& c : g_clients) {
        if (!c.alive || c.pad < 0 || c.pad >= 4) continue;
        if (!g_padConn[c.pad]) continue;        // pad unplugged -> no input (manager rebinds it on replug)
        XINPUT_STATE xs;
        if (XInputGetState((DWORD)c.pad, &xs) != ERROR_SUCCESS) continue;
        if (xs.Gamepad.wButtons & XINPUT_GAMEPAD_BACK) {        // hold BACK to leave
            if (!g_backHold[c.pad]) g_backHold[c.pad] = timeGetTime();
            else if (timeGetTime() - g_backHold[c.pad] > 1200) { g_backHold[c.pad] = 0; c.alive = false; CloseClient(c); continue; }
        } else g_backHold[c.pad] = 0;
        const short DZ = 7000;
        WriteAxis(c, AX_Forward,  StickAxis(xs.Gamepad.sThumbLY, DZ));   // left stick = move (analog)
        WriteAxis(c, AX_Side,    -StickAxis(xs.Gamepad.sThumbLX, DZ));
        WriteAxis(c, AX_Yaw,   (g_invPadX ?  1.f : -1.f) * StickAxis(xs.Gamepad.sThumbRX, DZ));  // turn
        WriteAxis(c, AX_Pitch, (g_invPadY ? -1.f :  1.f) * StickAxis(xs.Gamepad.sThumbRY, DZ));  // look
        PadState& s = g_pad[c.pad];
        WORD btn = xs.Gamepad.wButtons, changed = btn ^ s.prevBtn;
        for (int bit = 0; bit < 16; ++bit)
            if ((changed & (1 << bit)) && bit != 10 && bit != 11)   // bits 10,11 unused in XInput
                PushButton(c, 0x1B4 + bit, (btn >> bit) & 1);       // KEY_PAD_DPAD_UP + bit
        s.prevBtn = btn;
        bool lt = xs.Gamepad.bLeftTrigger > 64, rt = xs.Gamepad.bRightTrigger > 64;
        if (lt != s.lt) { PushButton(c, 0x1BE, lt ? 1 : 0); s.lt = lt; }   // KEY_PAD_LTRIGGER
        if (rt != s.rt) { PushButton(c, 0x1BF, rt ? 1 : 0); s.rt = rt; }   // KEY_PAD_RTRIGGER
    }
    LeaveCriticalSection(&g_clientsLock);
}

// --- composite all clients into the window via core/ layout. The back buffer (DC + bitmap) is
//     cached across frames and only rebuilt on resize, so the hot path is just the per-pane blits. ---
static void Render(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0) return;

    static HDC memDC = nullptr; static HBITMAP memBmp = nullptr; static int mw = 0, mh = 0;
    HDC hdc = GetDC(hwnd);
    if (!memDC) memDC = CreateCompatibleDC(hdc);
    if (W != mw || H != mh) {
        if (memBmp) DeleteObject(memBmp);
        memBmp = CreateCompatibleBitmap(hdc, W, H);
        SelectObject(memDC, memBmp);
        SetStretchBltMode(memDC, COLORONCOLOR);   // fast; at native pane res the blit is 1:1 anyway, and
                                                  // HALFTONE was very slow per frame for little gain
        mw = W; mh = H;
    }
    RECT full = { 0, 0, W, H };
    FillRect(memDC, &full, (HBRUSH)GetStockObject(BLACK_BRUSH));

    EnterCriticalSection(&g_clientsLock);
    int live = LiveCount();
    ss::Layout lay = ss::ComputeLayout(live, W, H, ss::TwoMode::Auto);
    bool drew = false;
    int pane = 0;
    for (size_t i = 0; i < g_clients.size(); ++i) {
        if (!g_clients[i].alive || pane >= lay.count) continue;
        OpenFb(g_clients[i]);
        const unsigned char* v = g_clients[i].view;
        const int* hdr = v ? (const int*)v : nullptr;
        if (hdr && hdr[0] == FB_MAGIC && hdr[1] > 0 && hdr[2] > 0) {
            int fw = hdr[1], fh = hdr[2];
            ss::Rect box = ss::Letterbox(lay.panes[pane], fw, fh);
            BITMAPINFO bi = { 0 };
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = fw;
            bi.bmiHeader.biHeight = fh;             // positive = bottom-up, matching glReadPixels (BGRA)
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;           // BGRA; the X byte is ignored under BI_RGB
            bi.bmiHeader.biCompression = BI_RGB;
            StretchDIBits(memDC, box.x, box.y, box.w, box.h, 0, 0, fw, fh, v + 16, &bi, DIB_RGB_COLORS, SRCCOPY);
            drew = true;
        }
        pane++;
    }
    LeaveCriticalSection(&g_clientsLock);
    if (!drew) {   // nothing rendering yet -- show a loading message instead of a black window
        char msg[80];
        _snprintf(msg, sizeof(msg), "Starting splitdronum...");
        SetBkMode(memDC, TRANSPARENT);
        SetTextColor(memDC, RGB(210, 210, 210));
        DrawTextA(memDC, msg, -1, &full, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    BitBlt(hdc, 0, 0, W, H, memDC, 0, 0, SRCCOPY);
    ReleaseDC(hwnd, hdc);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_ACTIVATE:
        if ((w & 0xFFFF) != WA_INACTIVE) {   // regained activation (alt-tab back / click on the window)
            // Raw mouse only flows to the FOREGROUND window and must be re-armed after a focus loss, and
            // keyboard focus can drift -- re-assert both so seat-0 input resumes immediately on tab-back.
            RAWINPUTDEVICE rid = { 0x01, 0x02, 0, h };
            RegisterRawInputDevices(&rid, 1, sizeof(rid));
            SetFocus(h);
        } else {                             // lost activation (alt-tab away): drop any keys held into the
            ReleaseSeat0Keys();              // switch so they don't stick down (the actual alt-tab bug)
        }
        break;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
    case WM_KEYUP:   case WM_SYSKEYUP: {
        if (g_clients.empty()) break;
        int sc = (int)((l >> 16) & 0xFF);     // hardware scancode the OS already gives us
        if (l & (1 << 24)) sc |= 0x80;        // extended key -> DIK high bit (arrows, RCtrl, ...)
        int gk = VkToGuiKey((int)w);
        int down = (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) ? 1 : 0;
        PushKey(g_clients[0], sc, gk, 0, down);   // seat 0 = keyboard/mouse
        return 0;
    }
    case WM_CHAR: case WM_SYSCHAR:
        if (!g_clients.empty() && w >= ' ') PushKey(g_clients[0], 0, 0, (int)w, 1);
        return 0;
    case WM_INPUT: {                          // raw mouse -> seat 0 look (engine applies sensitivity)
        UINT sz = 0;
        GetRawInputData((HRAWINPUT)l, RID_INPUT, NULL, &sz, sizeof(RAWINPUTHEADER));
        if (sz == 0 || sz > 1024) break;
        BYTE buf[1024];
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, buf, &sz, sizeof(RAWINPUTHEADER)) != sz) break;
        RAWINPUT* ri = (RAWINPUT*)buf;
        if (ri->header.dwType == RIM_TYPEMOUSE && !g_clients.empty() && !g_mouseFreed) {
            int dx = ri->data.mouse.lLastX, dy = ri->data.mouse.lLastY;
            // raw lLastY is +down and EV_Mouse y+ also looks down, so the default negates dy (mouse
            // up = look up). -MouseInvertY flips it.
            if (dx || dy) PushMouse(g_clients[0], dx, g_invMouseY ? dy : -dy);
            // mouse buttons -> key events with KEY_MOUSE1..3 scancodes (0x100..0x102). The DLL routes
            // them as fire/binds in game and as clicks in the menu. Without this, seat 0 can't fire.
            USHORT bf = ri->data.mouse.usButtonFlags;
            if (bf & RI_MOUSE_LEFT_BUTTON_DOWN)   PushKey(g_clients[0], 0x100, 0, 0, 1);
            if (bf & RI_MOUSE_LEFT_BUTTON_UP)     PushKey(g_clients[0], 0x100, 0, 0, 0);
            if (bf & RI_MOUSE_RIGHT_BUTTON_DOWN)  PushKey(g_clients[0], 0x101, 0, 0, 1);
            if (bf & RI_MOUSE_RIGHT_BUTTON_UP)    PushKey(g_clients[0], 0x101, 0, 0, 0);
            if (bf & RI_MOUSE_MIDDLE_BUTTON_DOWN) PushKey(g_clients[0], 0x102, 0, 0, 1);
            if (bf & RI_MOUSE_MIDDLE_BUTTON_UP)   PushKey(g_clients[0], 0x102, 0, 0, 0);
        }
        break;   // fall through so DefWindowProc does cleanup
    }
    }
    return DefWindowProcA(h, m, w, l);
}

static void KillChildren() {
    for (DWORD pid : g_hidePids) {
        HANDLE hp = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (hp) { TerminateProcess(hp, 0); CloseHandle(hp); }
    }
}

// --- desktop gamma guard ---------------------------------------------------------------------
// Each stock client sets a *global* hardware gamma ramp (SetDeviceGammaRamp) and only restores it
// in a destructor on a clean exit. We TerminateProcess the clients, so that destructor never runs
// and the desktop is left wearing the game's gamma curve. Fix it at the wrapper level (no engine
// edit): snapshot the ramp before any client launches, and put it back on every way out -- normal
// quit, Ctrl+C / console close / logoff, and an unhandled host crash.
static WORD g_origGamma[768];
static bool g_gammaSaved = false;

static void SaveGamma() {
    HDC dc = GetDC(NULL);                       // NULL = the screen DC, the same ramp the engine touches
    if (dc) { g_gammaSaved = !!GetDeviceGammaRamp(dc, g_origGamma); ReleaseDC(NULL, dc); }
}

static void RestoreGamma() {
    if (!g_gammaSaved) return;                  // idempotent: safe to call from several exit paths
    HDC dc = GetDC(NULL);
    if (dc) { SetDeviceGammaRamp(dc, g_origGamma); ReleaseDC(NULL, dc); }
}

// console close / Ctrl+C / logoff / shutdown: kill children + restore the ramp before we go.
static BOOL WINAPI CtrlHandler(DWORD) { KillChildren(); RestoreGamma(); return FALSE; }

// host crashed: restore the desktop gamma and orphan-kill the clients, then let the process die.
static LONG WINAPI CrashHandler(EXCEPTION_POINTERS*) {
    RestoreGamma(); KillChildren();
    return EXCEPTION_EXECUTE_HANDLER;
}

// --- dynamic drop-in: the manager thread brings up the server + seat 0, then spawns/despawns seats
//     live as controllers join (Start -> confirm card -> A) and leave (hold Back). The compositor
//     window stays up the whole time; clients are born hidden (DLL CreateWindowEx hook) so none steal
//     focus. A multi-word bind can't ride the command line, so the join binds go via splitseat.cfg. ---
static void DeploySeatCfg() {
    // Copy the user-editable global_seats.cfg verbatim to splitseat.cfg (every client +exec's it). If
    // it's missing, fall back to the essential controller-join binds so a stock launch still works.
    char cfgPath[600];
    _snprintf(cfgPath, sizeof(cfgPath), "%s\\splitseat.cfg", g_launch.cwd);
    FILE* out = fopen(cfgPath, "w");
    if (!out) return;
    FILE* in = g_launch.seatsCfg ? fopen(g_launch.seatsCfg, "rb") : NULL;
    if (in) {
        char buf[2048]; size_t r;
        while ((r = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, r, out);
        fclose(in);
    } else {
        fputs("bind pad_b \"ifspectator menu_join\"\n", out);
        fputs("bind pad_x \"ifspectator menu_join\"\n", out);
    }
    fclose(out);
}

// Spawn one stock client, connect it to the already-running loopback server, and add it as a seat.
// pad = the XInput index that drives it (-1 = seat 0, keyboard+mouse). The slow inject/resume runs
// WITHOUT the lock so the compositor never freezes during a join; only the brief append takes it.
// True if any LIVE seat already loaded the profile named `name`. Two seats can't run the same profile at
// once, so the create screen blocks confirming a name that's already in use.
static bool NameInUse(const char* name) {
    bool used = false;
    EnterCriticalSection(&g_clientsLock);
    for (auto& c : g_clients) if (c.alive && _stricmp(c.name, name) == 0) { used = true; break; }
    LeaveCriticalSection(&g_clientsLock);
    return used;
}

// Write the generated profile to <gamedir>/profiles/<name>.cfg (the seat +exec's it). core/ss_profile
// builds the body (name + crosshair, and movebob 0 when motion-sickness comp is on).
static void WriteProfileCfg(const char* name, int crosshair, int motionComp) {
    char dir[600]; _snprintf(dir, sizeof(dir), "%s\\profiles", g_launch.cwd); dir[sizeof(dir) - 1] = '\0';
    CreateDirectoryA(dir, NULL);
    char path[700]; _snprintf(path, sizeof(path), "%s\\%s.cfg", dir, name); path[sizeof(path) - 1] = '\0';
    char body[512]; ss::ProfileBuildCfg(body, sizeof(body), name, crosshair, motionComp);
    FILE* f = fopen(path, "w");
    if (f) { fputs(body, f); fclose(f); }
}

// pad = the XInput slot driving the seat (-1 = seat 0, keyboard+mouse). profile = a profiles/<name>.cfg
// display name (sets its own +name + execs its cfg), or NULL for the default P<seat> identity.
// This seat's render resolution for `liveCount` active seats: monitor size * base -RenderScale *
// SmartScale(count) -- smart scaling renders (and reads back) fewer pixels as more seats join, clamped
// to a usable window. SS_SMARTSCALE=0 falls back to the fixed base scale.
static void DesiredRes(int liveCount, int& w, int& h) {
    // base = monitor * user -RenderScale, capped first so 1-player cost stays bounded even on a huge
    // display; THEN smart-scale DOWN by seat count (capping after would make every count clamp to the
    // same max on >1080p monitors and defeat the point).
    int bw = (int)(GetSystemMetrics(SM_CXSCREEN) * g_renderScale);
    int bh = (int)(GetSystemMetrics(SM_CYSCREEN) * g_renderScale);
    if (bw > 1920) bw = 1920;  if (bh > 1080) bh = 1080;   // ceil: bound per-client cost
    float s = g_smartScale ? ss::SmartScale(liveCount) : 1.0f;
    w = (int)(bw * s);  h = (int)(bh * s);
    if (w < 640) w = 640;  if (h < 360) h = 360;           // floor: usable minimum
}

// Each seat launches at the smart-scaled resolution for the session's player count and KEEPS it for its
// lifetime -- changing an injected client's video mode at runtime hangs it (vid_setmode tears down the
// GL context/window while our hide-thread + swap hook are live), so we pick the res once, up front. For
// an `-Players N` session every seat renders at SmartScale(N); a later drop-in uses its own join count.
static bool SpawnClient(int pad, const char* profile) {
    // The SLOT index IS the seat's identity: seat 0 (kbd/mouse) is always slot 0 (-> seat0.ini, +name P1,
    // music owner); controllers take the lowest free slot 1..3. Stable + unique across quits/rejoins, so a
    // rejoining seat never collides with another seat's config or loses its seat-0 music role. (Computing
    // the index from the live count instead made a rejoin land on a dead seat's identity.)
    int slot = -1;
    EnterCriticalSection(&g_clientsLock);
    if (pad < 0) slot = 0;
    else {
        for (size_t i = 1; i < g_clients.size() && slot < 0; ++i) if (!g_clients[i].alive) slot = (int)i;
        if (slot < 0 && g_clients.size() < 4) { slot = (int)g_clients.size(); if (slot < 1) slot = 1; }
    }
    int liveCount = LiveCount();
    LeaveCriticalSection(&g_clientsLock);
    if (slot < 0 || slot > 3) return false;   // window full
    int seat = slot;
    int target = g_launch.n > liveCount + 1 ? g_launch.n : liveCount + 1;   // the -Players N batch shares a res
    if (target > 4) target = 4;
    // ss_profile bakes in the seat's identity/music and re-clamps the res. (GetSystemMetrics = host glue.)
    int mw, mh; DesiredRes(target, mw, mh);
    char args[820];
    ss::BuildClientArgs(args, sizeof(args), seat, g_launch.iwad, g_port, mw, mh, profile);
    Launch L = InjectSuspended(g_launch.dll, g_launch.exe, g_launch.cwd, args);
    DWORD pid = ResumeAfterHooks(L);
    if (!pid) return false;
    Client c; c.pid = pid; c.pad = pad; c.alive = true;
    c.hproc = OpenProcess(SYNCHRONIZE, FALSE, pid);   // watch for quit-game/crash (ReapDeadSeats below)
    if (profile && profile[0]) _snprintf(c.name, sizeof(c.name), "%s", profile);   // its profile name (dedup)
    else                       _snprintf(c.name, sizeof(c.name), "P%d", seat + 1); // default identity
    c.name[sizeof(c.name) - 1] = '\0';
    CreateSeatIn(c);
    EnterCriticalSection(&g_clientsLock);
    while ((int)g_clients.size() <= slot) g_clients.push_back(Client());   // grow to the slot (dead placeholders)
    g_clients[slot] = c;
    LeaveCriticalSection(&g_clientsLock);
    g_hidePids.push_back(pid);
    InterlockedIncrement(&g_launchN);
    return true;
}

// True if INADDR_ANY:port can't be bound right now -- i.e. someone holds it. Binding INADDR_ANY matches
// the stock server's own bind, so this is an accurate "free?" test (loopback-only would miss conflicts).
// Assumes WSAStartup has already run.
static bool PortInUse(int port) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons((u_short)port); a.sin_addr.s_addr = htonl(INADDR_ANY);
    int r = bind(s, (sockaddr*)&a, sizeof(a));
    closesocket(s);
    return r != 0;   // bind failed -> in use
}

// First bindable port at or above `start` (Zandronum's DEFAULT_SERVER_PORT = 10666). Falls back to
// `start` if the whole window is busy. Assumes WSAStartup has already run.
static int PickFreeUdpPort(int start) {
    for (int p = start; p < start + 64; ++p)
        if (!PortInUse(p)) return p;
    return start;
}

static DWORD WINAPI ManagerThread(LPVOID) {
    DeploySeatCfg();
    // The stock server FATAL-ERRORS if its port is busy (BindToLocalPort -> I_FatalError, no fallback). So:
    // probe a free port, host on it, and CONFIRM the server actually bound it before launching any client.
    // If the server lost the port to a race or crashed, re-probe + relaunch -- clients then never sit
    // forever retrying a server that isn't there (the old "stuck connecting" bug).
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    DWORD srvPid = 0;
    for (int attempt = 0; attempt < 4 && !srvPid; ++attempt) {
        g_port = PickFreeUdpPort(10666 + attempt);   // step the base each retry so a wedged port isn't re-picked
        char srvArgs[600];
        _snprintf(srvArgs, sizeof(srvArgs),
            "-host -iwad %s -port %d -config splitdronum-server.ini +map MAP01 +set sv_cooperative 1 +set sv_maxclients 8 "
            "+set sv_maxplayers 8 +set sv_maxclientsperip 8 +set sv_updatemaster 0 +set fullscreen 0", g_launch.iwad, g_port);
        Launch srv = InjectSuspended(g_launch.dll, g_launch.exe, g_launch.cwd, srvArgs);
        DWORD pid = ResumeAfterHooks(srv);
        bool up = false;
        for (int i = 0; i < 150 && !up; ++i) { Sleep(100); up = PortInUse(g_port); }   // wait <=15s for it to bind g_port
        if (up) srvPid = pid;                                                           // confirmed hosting on g_port
        else if (pid) { HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid); if (h) { TerminateProcess(h, 0); CloseHandle(h); } }
    }
    if (srvPid) g_hidePids.push_back(srvPid);

    g_clients.reserve(4);               // pin storage (<=4 seats) so the lock-free input path never races a realloc
    SpawnClient(-1, nullptr);           // seat 0 = keyboard + mouse (fullscreen)
    int initial = g_launch.n; if (initial > 4) initial = 4;
    for (int s = 1; s < initial; ++s) SpawnClient(s - 1, nullptr);   // CLI -Players N: pre-assign pads 0..N-2 for testing

    // Drop-in loop: feed assigned pads; watch UNassigned pads for Start -> confirm card -> A to join,
    // B / timeout to cancel. The compositor re-tiles automatically (layout is a function of live count).
    DWORD padCheck[4] = { 0, 0, 0, 0 };   // last poll of a DISCONNECTED slot (throttle the slow empty-slot query)
    WORD  joinPrev[4] = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };   // join-button edge-detect; 0xFFFF = not yet baselined
    DWORD bannerUntil = 0;                // the active join flow aborts once this idle deadline passes
    ss::JoinState js;                     // the (tested) join state machine's state; published to g_join* below
    bool seat0EnterPrev = false;        // edge-detect Enter for the keyboard-player rejoin
    for (;;) {
        FeedGameplay();
        ReapDeadSeats();                // free seats that quit-game or crashed on their own
        UpdateMusicOwner();             // keep exactly one seat on music (hand off when seat 0 quits/rejoins)
        DWORD now = timeGetTime();

        // Seat 0 (keyboard/mouse) rejoin -- a DIFFERENT path than controllers (no pad, no START button):
        // once it has quit/crashed, pressing Enter while the host is focused respawns it (back in slot 0)
        // with its default identity + the global config, the keyboard analog of a controller pressing Start.
        bool seat0Enter = (g_hwnd && GetForegroundWindow() == g_hwnd) && (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
        if (seat0Enter && !seat0EnterPrev && !Seat0Alive() && LiveCountLocked() < 4) SpawnClient(-1, nullptr);
        seat0EnterPrev = seat0Enter;

        // Refresh connection: poll a connected slot every loop, a disconnected one only ~1/s. XInput's
        // empty-slot query is slow, so hammering all four every frame is the documented perf hit.
        for (int p = 0; p < 4; ++p) {
            if (g_padConn[p] || now - padCheck[p] > 1000) {
                XINPUT_STATE xs; bool conn = (XInputGetState(p, &xs) == ERROR_SUCCESS);
                g_padConn[p] = conn; if (!conn) padCheck[p] = now;
            }
        }

        // Reconnect: a controller that came back -- possibly on a NEW slot index -- rebinds to the seat
        // whose pad went away, so unplug/replug just resumes that seat instead of spawning a duplicate
        // or stranding the player. (One waiting seat + one free pad covers the single-controller case.)
        int waitingSeat = -1;
        EnterCriticalSection(&g_clientsLock);
        for (size_t i = 0; i < g_clients.size(); ++i)
            if (g_clients[i].alive && g_clients[i].pad >= 0 && g_clients[i].pad < 4 && !g_padConn[g_clients[i].pad]) { waitingSeat = (int)i; break; }
        LeaveCriticalSection(&g_clientsLock);
        if (waitingSeat >= 0) {
            int freePad = -1;
            for (int p = 0; p < 4; ++p) if (g_padConn[p] && !PadAssigned(p)) { freePad = p; break; }
            if (freePad >= 0) { EnterCriticalSection(&g_clientsLock); g_clients[waitingSeat].pad = freePad; LeaveCriticalSection(&g_clientsLock); }
        }

        // Join: any UNassigned, connected controller that presses ANY (newly-pressed) button joins as a
        // new seat -- so it's obvious which pad joined, with no specific button to hunt for. The overlay
        // (g_joinPad) names the free controller, or invites a press when several are free.
        // Create flow: the host reads XInput + edge-detects buttons; core/ss_join (100%-tested) decides the
        // transitions. START opens the prompt; A/X/Y advances through name-word-1, name-word-2, crosshair,
        // motion-comp and finally creates+joins; dpad L/R scrolls the current field; B backs out a step;
        // unplug or a 20s idle deadline aborts. (sticks/shoulders never start it -- only a fresh START.)
        {
            bool room = LiveCountLocked() < 4;
            bool taken = false;   // 1 = the currently-composed name is already loaded (create blocked)
            if (js.pad < 0) {
                for (int p = 0; p < 4; ++p) {            // idle: scan free pads for a fresh START press
                    if (!g_padConn[p] || PadAssigned(p)) { joinPrev[p] = 0xFFFF; continue; }
                    XINPUT_STATE xs;
                    if (XInputGetState(p, &xs) != ERROR_SUCCESS) { joinPrev[p] = 0xFFFF; continue; }
                    WORD b = xs.Gamepad.wButtons, prev = joinPrev[p]; joinPrev[p] = b;
                    bool start = (prev != 0xFFFF) && (b & XINPUT_GAMEPAD_START & ~prev) != 0;
                    if (ss::JoinTryStart(js, p, start, room)) { bannerUntil = now + 20000; break; }
                }
            } else {
                XINPUT_STATE xs;
                bool ok = (js.pad < 4) && g_padConn[js.pad] && (XInputGetState((DWORD)js.pad, &xs) == ERROR_SUCCESS);
                WORD b = ok ? xs.Gamepad.wButtons : 0, prev = joinPrev[js.pad]; joinPrev[js.pad] = b;
                WORD nb = b & ~prev;                     // newly pressed this frame
                if (nb) bannerUntil = now + 20000;       // any input keeps the flow alive
                ss::JoinInput in;
                in.newButtons = ((nb & XINPUT_GAMEPAD_START) ? ss::JB_START : 0)
                              | ((nb & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y)) ? ss::JB_CONFIRM : 0)
                              | ((nb & XINPUT_GAMEPAD_B) ? ss::JB_CANCEL : 0)
                              | ((nb & XINPUT_GAMEPAD_DPAD_LEFT)  ? ss::JB_LEFT  : 0)
                              | ((nb & XINPUT_GAMEPAD_DPAD_RIGHT) ? ss::JB_RIGHT : 0);
                in.connected      = ok;
                in.timedOut       = (now > bannerUntil);
                in.word1Count     = ss::ProfileWord1Count();
                in.word2Count     = ss::ProfileWord2Count();
                in.crosshairCount = ss::kCrosshairMax + 1;
                char curName[64]; ss::ProfileComposeName(curName, sizeof(curName), js.word1, js.word2);
                in.nameInUse = NameInUse(curName);       // two seats can't load the same profile at once
                taken = in.nameInUse;
                ss::JoinResult r = ss::JoinAdvance(js, in);
                if (r.action == ss::JoinAction::Join) {
                    char nm[64]; ss::ProfileComposeName(nm, sizeof(nm), r.word1, r.word2);
                    WriteProfileCfg(nm, r.crosshair, r.motionComp);   // save profiles/<name>.cfg, then spawn it
                    SpawnClient(r.pad, nm);
                }
            }
            InterlockedExchange(&g_joinPad, js.pad);     // publish for the render thread (DrawJoinPrompt reads these)
            g_joinStep = js.step; g_joinW1 = js.word1; g_joinW2 = js.word2;
            g_joinCross = js.crosshair; g_joinMotion = js.motionComp; g_joinTaken = taken;
        }
        Sleep(8);
    }
    return 0;
}

// Composite on its own thread so the main thread is free to pump mouse/keyboard WM_INPUT at the
// input's own rate (~1kHz) instead of once per composited frame -- that's what makes seat-0 look
// as smooth as a native client, rather than stepping at the frame rate.
// --- GPU compositor (D3D11) -------------------------------------------------------------------
// Replaces the GDI path: each client framebuffer is uploaded as a texture and drawn as a quad into
// its pane via the GPU, then flipped to screen through a swap chain. GDI's StretchDIBits + BitBlt
// scaled with the window size and choked pushing a large surface at high refresh; the GPU doesn't.
static ID3D11Device*        g_dev   = nullptr;
static ID3D11DeviceContext* g_ctx   = nullptr;
static IDXGISwapChain*      g_swap  = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static ID3D11VertexShader*  g_vs    = nullptr;
static ID3D11PixelShader*   g_ps    = nullptr;
static ID3D11SamplerState*  g_samp  = nullptr;
static ID3D11RasterizerState* g_raster = nullptr;
static int g_swapW = 0, g_swapH = 0;
struct GTex { ID3D11Texture2D* tex; ID3D11ShaderResourceView* srv; int w, h; };
static GTex g_gtex[4] = { 0 };

static const char* g_hlsl =
"struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
"VSOut VSMain(uint id : SV_VertexID) {\n"
"  VSOut o;\n"
"  float2 p = float2((id==1||id==3)?1.0:0.0, (id==2||id==3)?1.0:0.0);\n"
"  o.pos = float4(p.x*2.0-1.0, 1.0-p.y*2.0, 0.0, 1.0);\n"
"  o.uv  = float2(p.x, 1.0-p.y);\n"   // flip V: the glReadPixels framebuffer is bottom-up
"  return o;\n}\n"
"Texture2D tx : register(t0); SamplerState sm : register(s0);\n"
"float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return tx.Sample(sm, uv); }\n";

static void MakeRTV() {
    ID3D11Texture2D* bb = nullptr;
    if (SUCCEEDED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) {
        g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
        bb->Release();
    }
}

static bool InitD3D(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right > 0 ? rc.right : 16, H = rc.bottom > 0 ? rc.bottom : 16;
    DXGI_SWAP_CHAIN_DESC sd = { 0 };
    sd.BufferCount = 2;
    sd.BufferDesc.Width = W; sd.BufferDesc.Height = H;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;   // efficient flip model, low DWM overhead
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &fl, &g_ctx);
    if (FAILED(hr))
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &fl, &g_ctx);
    if (FAILED(hr)) return false;
    g_swapW = W; g_swapH = H;
    MakeRTV();
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(g_hlsl, strlen(g_hlsl), nullptr, nullptr, nullptr, "VSMain", "vs_4_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(g_hlsl, strlen(g_hlsl), nullptr, nullptr, nullptr, "PSMain", "ps_4_0", 0, 0, &psb, &err)))
        return false;
    g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
    g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);
    vsb->Release(); psb->Release();
    D3D11_SAMPLER_DESC smp = { 0 }; smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; smp.MaxLOD = D3D11_FLOAT32_MAX;
    g_dev->CreateSamplerState(&smp, &g_samp);
    D3D11_RASTERIZER_DESC rd = { D3D11_FILL_SOLID, D3D11_CULL_NONE };
    g_dev->CreateRasterizerState(&rd, &g_raster);
    // don't let DXGI hook the message-thread window (Alt+Enter etc.)
    IDXGIDevice* dxd = nullptr; IDXGIAdapter* ad = nullptr; IDXGIFactory* fac = nullptr;
    if (SUCCEEDED(g_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxd))) {
        if (SUCCEEDED(dxd->GetAdapter(&ad))) {
            if (SUCCEEDED(ad->GetParent(__uuidof(IDXGIFactory), (void**)&fac))) {
                fac->MakeWindowAssociation(hwnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
                fac->Release();
            }
            ad->Release();
        }
        dxd->Release();
    }
    return true;
}

// The create-and-join card. The D3D compositor has no font system, so GDI-render the current step (name
// words / crosshair / motion-comp) into a DIB, rebuilt only when the shown state changes, upload it as a
// texture, and draw it centered. Assumes the caller bound the shader/sampler/raster/topology (RenderD3D).
static ID3D11Texture2D*          g_promptTex = nullptr;
static ID3D11ShaderResourceView* g_promptSrv = nullptr;
static int g_promptN = -1, g_promptW = 0, g_promptH = 0;
static void DrawJoinPrompt(ss::Rect area) {
    LONG jp = InterlockedCompareExchange(&g_joinPad, -1, -1);
    if (jp < 0) return;
    int step = (int)g_joinStep, w1 = (int)g_joinW1, w2 = (int)g_joinW2;
    int cross = (int)g_joinCross, motion = (int)g_joinMotion, taken = (int)g_joinTaken;
    int key = ((((((int)jp * 8 + step) * 16 + w1) * 16 + w2) * 16 + cross) * 2 + motion) * 2 + taken;
    if (key != g_promptN || !g_promptSrv) {
        g_promptN = key;
        if (g_promptSrv) { g_promptSrv->Release(); g_promptSrv = nullptr; }
        if (g_promptTex) { g_promptTex->Release(); g_promptTex = nullptr; }
        const int TW = 660, TH = 152; g_promptW = TW; g_promptH = TH;
        BITMAPINFO bi = { 0 };
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = TW; bi.bmiHeader.biHeight = TH;   // bottom-up to match the shader's V-flip
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(NULL);
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        HGDIOBJ ob = SelectObject(dc, bmp);
        RECT r = { 0, 0, TW, TH };
        HBRUSH bg = CreateSolidBrush(RGB(24, 26, 34)); FillRect(dc, &r, bg); DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        HFONT ft = CreateFontA(32, 0, 0, 0, FW_BOLD,   0, 0, 0, 0, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
        HFONT fb = CreateFontA(40, 0, 0, 0, FW_BOLD,   0, 0, 0, 0, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
        HFONT fs = CreateFontA(23, 0, 0, 0, FW_NORMAL, 0, 0, 0, 0, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
        char l1[48]; _snprintf(l1, sizeof(l1), "Controller %d", (int)jp + 1);
        HGDIOBJ of = SelectObject(dc, ft);
        SetTextColor(dc, RGB(236, 239, 250));
        RECT r1 = { 0, 14, TW, 52 }; DrawTextA(dc, l1, -1, &r1, DT_CENTER | DT_SINGLELINE);
        char nm[64]; ss::ProfileComposeName(nm, sizeof(nm), w1, w2);
        char mid[160]; const char* hint = "dpad change      A  next      B  back";
        if (step == ss::JS_PROMPT) {
            _snprintf(mid, sizeof(mid), "press  A  to make a player");  hint = "B  cancel";
        } else if (step == ss::JS_WORD1) {                // scrolling the first name word
            _snprintf(mid, sizeof(mid), "<  %s  >%s", ss::ProfileWord1(w1), ss::ProfileWord2(w2));
        } else if (step == ss::JS_WORD2) {                // scrolling the second name word
            _snprintf(mid, sizeof(mid), "%s<  %s  >", ss::ProfileWord1(w1), ss::ProfileWord2(w2));
        } else if (step == ss::JS_CROSSHAIR) {            // picking a crosshair
            _snprintf(mid, sizeof(mid), "%s    crosshair <  %d  >", nm, cross);
        } else {                                          // ss::JS_MOTION -- motion-sickness comp
            _snprintf(mid, sizeof(mid), "%s    motion comp <  %s  >", nm, motion ? "ON" : "off");
            hint = taken ? "name taken -- scroll to another      B  back" : "A  create + join      B  back";
        }
        SelectObject(dc, fb); SetTextColor(dc, taken ? RGB(255, 140, 140) : RGB(150, 220, 255));
        RECT r2 = { 0, 56, TW, 106 }; DrawTextA(dc, mid, -1, &r2, DT_CENTER | DT_SINGLELINE);
        SelectObject(dc, fs); SetTextColor(dc, RGB(140, 150, 170));
        RECT r3 = { 0, 114, TW, 146 }; DrawTextA(dc, hint, -1, &r3, DT_CENTER | DT_SINGLELINE);
        SelectObject(dc, of); DeleteObject(ft); DeleteObject(fb); DeleteObject(fs);
        unsigned char* px = (unsigned char*)bits;
        for (int i = 0; i < TW * TH; ++i) px[i * 4 + 3] = 255;   // GDI leaves alpha 0 -> make the card opaque
        D3D11_TEXTURE2D_DESC td = { 0 };
        td.Width = TW; td.Height = TH; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd = { bits, (UINT)(TW * 4), 0 };
        if (SUCCEEDED(g_dev->CreateTexture2D(&td, &sd, &g_promptTex)))
            g_dev->CreateShaderResourceView(g_promptTex, nullptr, &g_promptSrv);
        SelectObject(dc, ob); DeleteObject(bmp); DeleteDC(dc);
    }
    if (g_promptSrv) {
        int cx = area.x + (area.w - g_promptW) / 2;       // center the card within `area`:
        int cy = area.y + (area.h - g_promptH) / 2;       // a reserved pane while customizing, else whole window
        D3D11_VIEWPORT vp = { (float)cx, (float)cy, (float)g_promptW, (float)g_promptH, 0.0f, 1.0f };
        g_ctx->RSSetViewports(1, &vp);
        g_ctx->PSSetShaderResources(0, 1, &g_promptSrv);
        g_ctx->Draw(4, 0);
    }
}

// Generic cached text overlay: GDI-render a string to a texture once and reuse it by exact string.
// Used for the per-pane "controller disconnected" status (the join card has its own renderer above).
struct TextEntry { char msg[112]; ID3D11Texture2D* tex; ID3D11ShaderResourceView* srv; int w, h; };
static TextEntry g_textCache[8]; static int g_textCount = 0;
static ID3D11ShaderResourceView* TextTexture(const char* msg, int* pw, int* ph) {
    for (int i = 0; i < g_textCount; ++i)
        if (strcmp(g_textCache[i].msg, msg) == 0) { if (pw)*pw = g_textCache[i].w; if (ph)*ph = g_textCache[i].h; return g_textCache[i].srv; }
    const int TW = 640, TH = 52;
    BITMAPINFO bi = { 0 };
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = TW; bi.bmiHeader.biHeight = TH;   // bottom-up to match the shader's V-flip
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ ob = SelectObject(dc, bmp);
    RECT r = { 0, 0, TW, TH };
    HBRUSH bg = CreateSolidBrush(RGB(34, 16, 16)); FillRect(dc, &r, bg); DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    HFONT f = CreateFontA(26, 0, 0, 0, FW_BOLD, 0, 0, 0, 0, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
    HGDIOBJ of = SelectObject(dc, f);
    SetTextColor(dc, RGB(255, 198, 138));
    DrawTextA(dc, msg, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, of); DeleteObject(f);
    unsigned char* px = (unsigned char*)bits; for (int i = 0; i < TW * TH; ++i) px[i * 4 + 3] = 255;
    ID3D11Texture2D* tex = nullptr; ID3D11ShaderResourceView* srv = nullptr;
    D3D11_TEXTURE2D_DESC td = { 0 };
    td.Width = TW; td.Height = TH; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = { bits, (UINT)(TW * 4), 0 };
    if (SUCCEEDED(g_dev->CreateTexture2D(&td, &sd, &tex))) g_dev->CreateShaderResourceView(tex, nullptr, &srv);
    SelectObject(dc, ob); DeleteObject(bmp); DeleteDC(dc);
    if (g_textCount < 8 && srv) { TextEntry& e = g_textCache[g_textCount++]; _snprintf(e.msg, sizeof(e.msg), "%s", msg); e.tex = tex; e.srv = srv; e.w = TW; e.h = TH; }
    if (pw)*pw = TW; if (ph)*ph = TH;
    return srv;
}

static void RenderD3D(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0) return;
    if (W != g_swapW || H != g_swapH) {                 // window resized -> resize the swap chain
        if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
        g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
        g_swap->ResizeBuffers(0, W, H, DXGI_FORMAT_UNKNOWN, 0);
        g_swapW = W; g_swapH = H;
        MakeRTV();
    }
    float black[4] = { 0, 0, 0, 1 };
    g_ctx->ClearRenderTargetView(g_rtv, black);
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_ctx->VSSetShader(g_vs, nullptr, 0);
    g_ctx->PSSetShader(g_ps, nullptr, 0);
    g_ctx->PSSetSamplers(0, 1, &g_samp);
    g_ctx->RSSetState(g_raster);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    EnterCriticalSection(&g_clientsLock);
    int live = LiveCount();
    // A controller that confirmed the prompt and is customizing reserves its OWN pane up front, so the
    // create card sits in its panel (seat 0 shrinks beside it) instead of overlapping the live seats. The
    // pane stays put when the seat actually spawns on the final confirm -> no layout jump, no overlap.
    int joinPad  = (int)InterlockedCompareExchange(&g_joinPad, -1, -1);
    int joinStep = (int)g_joinStep;
    int pending  = (joinPad >= 0 && joinStep >= ss::JS_WORD1 && live < 4) ? 1 : 0;
    ss::Layout lay = ss::ComputeLayout(live + pending, W, H, ss::TwoMode::Auto);
    int pane = 0;
    for (size_t i = 0; i < g_clients.size() && i < 4; ++i) {
        if (!g_clients[i].alive || pane >= lay.count) continue;
        OpenFb(g_clients[i]);
        const unsigned char* v = g_clients[i].view;
        const int* hdr = v ? (const int*)v : nullptr;
        if (hdr && hdr[0] == FB_MAGIC && hdr[1] > 0 && hdr[2] > 0) {
            int fw = hdr[1], fh = hdr[2];
            GTex& t = g_gtex[i];
            if (!t.tex || t.w != fw || t.h != fh) {         // (re)create this seat's texture at its render res
                if (t.srv) { t.srv->Release(); t.srv = nullptr; }
                if (t.tex) { t.tex->Release(); t.tex = nullptr; }
                D3D11_TEXTURE2D_DESC td = { 0 };
                td.Width = fw; td.Height = fh; td.MipLevels = 1; td.ArraySize = 1;
                td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                if (SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &t.tex))) {
                    g_dev->CreateShaderResourceView(t.tex, nullptr, &t.srv); t.w = fw; t.h = fh;
                }
            }
            if (t.tex && t.srv) {
                D3D11_MAPPED_SUBRESOURCE ms;
                if (SUCCEEDED(g_ctx->Map(t.tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
                    const unsigned char* src = v + 16;          // BGRA, row pitch fw*4
                    for (int y = 0; y < fh; ++y)
                        memcpy((unsigned char*)ms.pData + (size_t)y * ms.RowPitch, src + (size_t)y * fw * 4, (size_t)fw * 4);
                    g_ctx->Unmap(t.tex, 0);
                }
                ss::Rect box = ss::Letterbox(lay.panes[pane], fw, fh);   // GPU scales into this seat's pane
                D3D11_VIEWPORT vp = { (float)box.x, (float)box.y, (float)box.w, (float)box.h, 0.0f, 1.0f };
                g_ctx->RSSetViewports(1, &vp);
                g_ctx->PSSetShaderResources(0, 1, &t.srv);
                g_ctx->Draw(4, 0);
            }
        }
        // controller unplugged -> status overlay on this seat's pane (reconnect resumes; hold Backspace to drop)
        if (g_clients[i].pad >= 0 && g_clients[i].pad < 4 && !g_padConn[g_clients[i].pad]) {
            char msg[112]; _snprintf(msg, sizeof(msg), "Player %d  -  controller disconnected   (hold Backspace to drop)", pane + 1);
            int tw, th; ID3D11ShaderResourceView* osrv = TextTexture(msg, &tw, &th);
            if (osrv) {
                ss::Rect pn = lay.panes[pane];
                int ow = tw, oh = th;
                if (ow > pn.w - 16) { oh = (int)((long long)oh * (pn.w - 16) / ow); ow = pn.w - 16; }   // fit width
                if (ow < 40) { ow = pn.w; oh = th; }
                D3D11_VIEWPORT vp = { (float)(pn.x + (pn.w - ow) / 2), (float)(pn.y + (pn.h - oh) / 2), (float)ow, (float)oh, 0.0f, 1.0f };
                g_ctx->RSSetViewports(1, &vp);
                g_ctx->PSSetShaderResources(0, 1, &osrv);
                g_ctx->Draw(4, 0);
            }
        }
        pane++;   // an alive seat reserves its pane even before its first frame (stable tiling on join)
    }
    LeaveCriticalSection(&g_clientsLock);
    // While customizing, draw the create card in the reserved pane (pane == live now); for the brief
    // "press A" prompt (no reserved pane) center it over the whole window.
    ss::Rect area = (pending && pane < lay.count) ? lay.panes[pane] : ss::Rect{ 0, 0, W, H };
    DrawJoinPrompt(area);
    if (!Seat0Alive()) {   // seat 0 (kbd/mouse) quit/crashed -- invite a keyboard rejoin (top-center banner)
        int tw, th; ID3D11ShaderResourceView* s = TextTexture("seat 0 left  -  press Enter to rejoin", &tw, &th);
        if (s) {
            D3D11_VIEWPORT vp = { (float)((W - tw) / 2), 24.0f, (float)tw, (float)th, 0.0f, 1.0f };
            g_ctx->RSSetViewports(1, &vp);
            g_ctx->PSSetShaderResources(0, 1, &s);
            g_ctx->Draw(4, 0);
        }
    }
    // backbuffer dump for verification: set SS_CAPTURE, then drop a "capture.now" file in the cwd.
    // Gated on the env once so there is zero per-frame cost in normal use.
    static int s_capOn = -1;
    if (s_capOn < 0) s_capOn = getenv("SS_CAPTURE") ? 1 : 0;
    if (s_capOn && GetFileAttributesA("capture.now") != INVALID_FILE_ATTRIBUTES) {
        DeleteFileA("capture.now");
        ID3D11Texture2D* bb = nullptr;
        if (SUCCEEDED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) {
            D3D11_TEXTURE2D_DESC bd; bb->GetDesc(&bd);
            bd.Usage = D3D11_USAGE_STAGING; bd.BindFlags = 0; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; bd.MiscFlags = 0;
            ID3D11Texture2D* stg = nullptr;
            if (SUCCEEDED(g_dev->CreateTexture2D(&bd, nullptr, &stg))) {
                g_ctx->CopyResource(stg, bb);
                D3D11_MAPPED_SUBRESOURCE ms;
                if (SUCCEEDED(g_ctx->Map(stg, 0, D3D11_MAP_READ, 0, &ms))) {
                    FILE* f = fopen("d3d_capture.raw", "wb");
                    if (f) { int w = (int)bd.Width, h = (int)bd.Height; fwrite(&w, 4, 1, f); fwrite(&h, 4, 1, f);
                             for (UINT y = 0; y < bd.Height; ++y) fwrite((unsigned char*)ms.pData + (size_t)y * ms.RowPitch, 4, bd.Width, f); fclose(f); }
                    g_ctx->Unmap(stg, 0);
                }
                stg->Release();
            }
            bb->Release();
        }
    }
    g_swap->Present(1, 0);   // vsync (replaces the GDI DwmFlush)
}

static DWORD WINAPI RenderThread(LPVOID) {
    DWORD fpsT0 = timeGetTime(); int frames = 0;
    const bool framelog = getenv("SS_FRAMELOG") != nullptr;   // diagnostic: log composite present cadence
    LARGE_INTEGER qf = { 0 }, qlast = { 0 }; QueryPerformanceFrequency(&qf);
    double ftSum = 0, ftMax = 0; int ftN = 0, ftStutter = 0; DWORD ftT0 = timeGetTime();
    while (!g_hwnd) Sleep(5);
    bool d3d = InitD3D(g_hwnd);   // GPU compositor; transparently falls back to GDI if D3D is unavailable
    for (;;) {
        if (g_hwnd) {
            if (d3d) RenderD3D(g_hwnd); else Render(g_hwnd);
            ++frames;
            DWORD now = timeGetTime();
            if (now - fpsT0 >= 1000) {
                char title[80];
                _snprintf(title, sizeof(title), "splitdronum  -  %d fps%s",
                          frames * 1000 / (int)(now - fpsT0), d3d ? "" : "  (GDI fallback)");
                SetWindowTextA(g_hwnd, title);
                frames = 0; fpsT0 = now;
            }
        }
        if (!d3d) { if (DwmFlush() != S_OK) Sleep(1); }   // D3D Present(1,0) already vsyncs
        if (framelog) {                     // measure what the USER sees: present-to-present interval
            LARGE_INTEGER qn; QueryPerformanceCounter(&qn);
            if (qlast.QuadPart) {
                double ms = (double)(qn.QuadPart - qlast.QuadPart) * 1000.0 / (double)qf.QuadPart;
                ftSum += ms; if (ms > ftMax) ftMax = ms; ++ftN; if (ms > 16.7) ++ftStutter;
            }
            qlast = qn;
            DWORD nt = timeGetTime();
            if (nt - ftT0 >= 3000 && ftN > 0) {
                FILE* f = fopen("host_frametimes.log", "a");
                if (f) { fprintf(f, "composite present: n=%d avg=%.2fms max=%.2fms stutters>16.7ms=%d\n",
                                 ftN, ftSum / ftN, ftMax, ftStutter); fclose(f); }
                ftSum = 0; ftMax = 0; ftN = 0; ftStutter = 0; ftT0 = nt;
            }
        }
    }
}

// Log any unhandled crash (code + faulting module + RVA) so a hard-to-repro hang/crash leaves a trail.
static LONG WINAPI CrashLog(EXCEPTION_POINTERS* ep) {
    FILE* f = fopen("host_crash.log", "a");
    if (f) {
        void* addr = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionAddress : nullptr;
        DWORD code = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionCode : 0;
        char modName[MAX_PATH] = "?"; HMODULE mod = NULL;
        if (addr && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &mod) && mod)
            GetModuleFileNameA(mod, modName, MAX_PATH);
        fprintf(f, "CRASH code=0x%08lx addr=%p rva=0x%llx module=%s\n", code, addr,
                mod ? (unsigned long long)((char*)addr - (char*)mod) : 0ULL, modName);
        fclose(f);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char** argv) {
    SetUnhandledExceptionFilter(CrashLog);
    if (argc < 5) {
        printf("usage: host <numPlayers> <ss_hook.dll> <zandronum.exe> <iwad> [gamedir]\n");
        return 1;
    }
    int n = atoi(argv[1]);
    if (n < 1) n = 1;
    if (n > 4) n = 4;
    const char* dll = argv[2];
    const char* exe = argv[3];
    const char* iwad = argv[4];
    const char* cwd = (argc > 5) ? argv[5] : NULL;
    const char* seatsCfg = (argc > 6) ? argv[6] : NULL;   // global config for all seats (global_seats.cfg)
    // Hide our own console so only the game window shows. Guard on GetConsoleProcessList: hide only
    // when host owns the console (count == 1, e.g. launched detached by play.ps1), never a shell's
    // console we were merely launched into from an interactive prompt (that would hide the user's terminal).
    {
        DWORD cpl[2];
        if (GetConsoleProcessList(cpl, 2) == 1) ShowWindow(GetConsoleWindow(), SW_HIDE);
    }
    g_invMouseY = getenv("SS_MOUSE_INVY") != NULL;   // optional look-invert toggles (play.ps1)
    g_invPadY   = getenv("SS_PAD_INVY")   != NULL;
    g_invPadX   = getenv("SS_PAD_INVX")   != NULL;
    { const char* rs = getenv("SS_RENDER_SCALE");
      if (rs && *rs) { g_renderScale = (float)atof(rs);
          if (g_renderScale < 0.25f) g_renderScale = 0.25f;     // floor: don't render absurdly low
          if (g_renderScale > 2.0f)  g_renderScale = 2.0f; } }  // ceil: supersample at most 2x
    { const char* sm = getenv("SS_SMARTSCALE"); if (sm && sm[0] == '0') g_smartScale = false; }  // opt out

    FILE* L = fopen("host_debug.log", "w");

    // Snapshot the desktop gamma BEFORE any client can change it, and arm the exit-path guards
    // (atexit covers normal returns; the ctrl + crash handlers cover the abnormal ways out).
    SaveGamma();
    atexit(RestoreGamma);
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    SetUnhandledExceptionFilter(CrashHandler);
    if (L) { fprintf(L, "gamma saved=%d\n", g_gammaSaved ? 1 : 0); fflush(L); }

    // Bring up the compositor window FIRST, so there is one stable, focused window from the start;
    // the server + clients then spawn on a background thread (born hidden, so they never steal focus).
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "SplitdronumHost";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    // App icon (resource ID 1 from host.rc) for the title bar + taskbar. The embedded .ico only sets
    // the EXE's file icon in Explorer; the *window/taskbar* icon comes from the class hIcon (large), so
    // load it from the same resource. The small (title-bar) icon is set via WM_SETICON after creation.
    HICON hIconBig = (HICON)LoadImageA(wc.hInstance, MAKEINTRESOURCE(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    HICON hIconSm  = (HICON)LoadImageA(wc.hInstance, MAKEINTRESOURCE(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    wc.hIcon = hIconBig;
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "splitdronum", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) { if (L) { fprintf(L, "CreateWindow failed err=%lu\n", GetLastError()); fflush(L); } return 2; }
    if (hIconBig) SendMessageA(hwnd, WM_SETICON, ICON_BIG,   (LPARAM)hIconBig);   // taskbar / Alt+Tab
    if (hIconSm)  SendMessageA(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSm);    // title bar
    ForceForeground(hwnd);   // robustly claim focus (plain SetForegroundWindow no-ops for our hidden launch)
    timeBeginPeriod(1);   // 1ms scheduler tick so the present loop runs uncapped, not ~64fps

    RAWINPUTDEVICE rid = { 0x01, 0x02, 0, hwnd };   // HID generic mouse -> WM_INPUT to this window
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
    ShowCursor(FALSE);

    // reserve so neither list ever reallocates while another thread reads it (seats <=4; hide-pids
    // accumulate across live join/leave, so reserve generously)
    g_clients.reserve(4);
    g_hidePids.reserve(64);
    InitializeCriticalSection(&g_clientsLock);   // guards the dynamic seat list (manager vs render/main)
    g_launch.dll = dll; g_launch.exe = exe; g_launch.iwad = iwad; g_launch.cwd = cwd; g_launch.n = n;
    g_launch.seatsCfg = seatsCfg;
    g_hwnd = hwnd;
    CreateThread(NULL, 0, RenderThread, NULL, 0, NULL);    // compositing runs off the main thread
    CreateThread(NULL, 0, ManagerThread, NULL, 0, NULL);   // server + seat 0, then live controller join/leave

    // Main thread = INPUT only, run tight (~1kHz) so mouse/keyboard WM_INPUT is pumped at the
    // device rate (smooth seat-0 look), not throttled to the composite frame rate. The pads /
    // foreground / cursor work doesn't need 1kHz, so it runs at ~250Hz to leave CPU for the render
    // thread and clients.
    MSG msg;
    DWORD lastSlow = timeGetTime();
    for (;;) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {   // mouse/keyboard at the full loop rate
            if (msg.message == WM_QUIT) { timeEndPeriod(1); KillChildren(); RestoreGamma(); return 0; }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        DWORD now = timeGetTime();
        if (now - lastSlow >= 4) {   // ~250Hz: pads, foreground reclaim, cursor clip
            lastSlow = now;
            // if one of our own hidden child windows grabbed foreground during the staggered
            // startup, take it back so input stays on seat 0 -- but yield to a real user alt-tab.
            HWND fg = GetForegroundWindow();
            if (fg != hwnd) {
                static DWORD s_start = now;                       // host launches hidden -> Windows can
                bool startupPhase = (now - s_start) < 6000;       // leave it un-foregrounded; force it
                DWORD fgPid = 0; GetWindowThreadProcessId(fg, &fgPid);
                bool childFg = false;
                for (DWORD p : g_hidePids) if (p == fgPid) { childFg = true; break; }
                // Reclaim from our own hidden child windows always (a startup race stole focus); during
                // the startup window, force it unconditionally so the host actually lands in front. Once
                // settled we leave a real foreign app alone (the user deliberately alt-tabbed away).
                if (childFg || startupPhase) ForceForeground(hwnd);
            }
            // (Controller pads are polled by the manager thread now -- it owns join/leave + gameplay.)
            // Hybrid mouse-free: release the mouse when seat 0 (the mouse user) is in a menu/console,
            // OR when every seat is (nobody is actively playing). The mouse is idle in that state
            // (m_use_mouse 0 means menus ignore it), so let it roam the desktop -- move the window, hit
            // the taskbar, alt-tab. The raw-input handler stops feeding seat 0 while freed (g_mouseFreed).
            bool freeMouse = false;
            EnterCriticalSection(&g_clientsLock);
            if (!g_clients.empty() && g_clients[0].in) {
                bool seat0Menu = (g_clients[0].in[IN_MENU] != 0);
                bool allMenu = true;
                for (auto& c : g_clients)
                    if (c.alive && !(c.in && c.in[IN_MENU] != 0)) { allMenu = false; break; }
                freeMouse = seat0Menu || allMenu;
            }
            LeaveCriticalSection(&g_clientsLock);
            if (freeMouse && !g_mouseFreed)      { g_mouseFreed = 1; ShowCursor(TRUE); }
            else if (!freeMouse && g_mouseFreed) { g_mouseFreed = 0; ShowCursor(FALSE); }
            // Hold Backspace (host focused) to DROP a seat whose controller is disconnected -- the
            // intentional-removal path, since you can't hold-Back-to-leave on an unplugged pad. Hold
            // (not tap) avoids accidents; one drop per hold (release to drop the next).
            {
                static DWORD bsDown = 0; static bool bsDropped = false;
                bool bsHeld = (GetForegroundWindow() == hwnd) && (GetAsyncKeyState(VK_BACK) & 0x8000) != 0;
                if (bsHeld) {
                    if (!bsDown) { bsDown = now; bsDropped = false; }
                    else if (!bsDropped && now - bsDown > 1000) {
                        EnterCriticalSection(&g_clientsLock);
                        for (auto& c : g_clients)
                            if (c.alive && c.pad >= 0 && c.pad < 4 && !g_padConn[c.pad]) { c.alive = false; CloseClient(c); break; }
                        LeaveCriticalSection(&g_clientsLock);
                        bsDropped = true;
                    }
                } else bsDown = 0;
            }
            if (GetForegroundWindow() == hwnd && !g_mouseFreed) {   // capture the mouse for seat-0 look
                RECT cr; GetClientRect(hwnd, &cr);
                POINT a = { 0, 0 }, b = { cr.right, cr.bottom };
                ClientToScreen(hwnd, &a); ClientToScreen(hwnd, &b);
                RECT clip = { a.x, a.y, b.x, b.y };
                ClipCursor(&clip);
            } else {
                ClipCursor(NULL);                  // alt-tabbed away, or freed for a menu -- let it roam
            }
        }
        Sleep(1);   // ~1kHz input loop (timeBeginPeriod)
    }
}
