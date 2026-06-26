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
static const int IN_AXES  = 136;          // ZanIN int index of the 5 controller axes (value x1000)
static const int IN_MENU  = 160;          // ZanIN int the DLL writes back: 1 = that seat is in a menu/console
static volatile LONG g_mouseFreed = 0;    // 1 = release the mouse (a menu is up) so it can roam the desktop
static float g_renderScale = 1.0f;        // client render res = monitor size * this (SS_RENDER_SCALE; <1 = faster/softer)
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
};
static std::vector<Client> g_clients;
static std::vector<DWORD>  g_hidePids;   // server + clients: their own windows are kept hidden

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

// push one key event into a seat's ring (a = (scancode<<1)|down, b = gk | (char<<16))
static void PushKey(Client& c, int sc, int gk, int ch, int down) {
    if (!c.in) return;
    int slot = c.keyWrite % KEY_RING;
    c.in[8 + slot * 2]     = (sc << 1) | (down & 1);
    c.in[8 + slot * 2 + 1] = (gk & 0xffff) | (ch << 16);
    ++c.keyWrite;
    c.in[5] = c.keyWrite;             // publish last (the DLL reads [5], then the entries)
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

// --- XInput pads drive seats 1..N-1 (controller index = seat-1). We route each pad's analog
//     sticks into the engine's OWN joystick axes (the DLL's I_GetAxes hook), so G_BuildTiccmd
//     does NATIVE analog movement + joystick look -- no keyboard/mouse emulation. Only buttons
//     stay edge-detected key events. Clients run use_joystick 0 so the shared physical pad
//     doesn't leak into every seat. ---
struct PadState { WORD prevBtn; bool lt, rt; };
static PadState g_pad[4];

static void PollControllers(int n) {
    for (int seat = 1; seat < n && seat < (int)g_clients.size(); ++seat) {
        XINPUT_STATE xs;
        if (XInputGetState((DWORD)(seat - 1), &xs) != ERROR_SUCCESS) continue;
        Client& c = g_clients[seat];
        const short DZ = 7000;
        WriteAxis(c, AX_Forward,  StickAxis(xs.Gamepad.sThumbLY, DZ));   // left stick = move (analog):
        WriteAxis(c, AX_Side,    -StickAxis(xs.Gamepad.sThumbLX, DZ));   //   up = forward, right = strafe right
        WriteAxis(c, AX_Yaw,   (g_invPadX ?  1.f : -1.f) * StickAxis(xs.Gamepad.sThumbRX, DZ));  // turn
        WriteAxis(c, AX_Pitch, (g_invPadY ? -1.f :  1.f) * StickAxis(xs.Gamepad.sThumbRY, DZ));  // look up/down
        // DEBUG: log the real pad axis signs while a stick is held, so any remaining inversion is
        // settled by measurement -- push the right stick UP / RIGHT, then read build\pad_debug.log.
        static int dbgN = 0;
        if ((abs(xs.Gamepad.sThumbRY) > 20000 || abs(xs.Gamepad.sThumbRX) > 20000) && (dbgN++ % 20 == 0)) {
            FILE* f = fopen("pad_debug.log", "a");
            if (f) { fprintf(f, "seat %d RX=%d RY=%d\n", seat, xs.Gamepad.sThumbRX, xs.Gamepad.sThumbRY); fclose(f); }
        }
        // buttons -> native KEY_PAD_* codes, so the player's OWN controller binds drive them
        // (all 16 face/dpad/shoulder/thumb buttons + the two analog triggers), game and menu.
        PadState& s = g_pad[seat];
        WORD btn = xs.Gamepad.wButtons, changed = btn ^ s.prevBtn;
        for (int bit = 0; bit < 16; ++bit)
            if ((changed & (1 << bit)) && bit != 10 && bit != 11)   // bits 10,11 unused in XInput
                PushButton(c, 0x1B4 + bit, (btn >> bit) & 1);       // KEY_PAD_DPAD_UP + bit
        // (Opening the spectator JOIN screen is handled by binding a spare pad button to the engine's
        //  own "menu_join" command at client launch -- see LaunchThread -- not by faking a keystroke.)
        s.prevBtn = btn;
        bool lt = xs.Gamepad.bLeftTrigger > 64, rt = xs.Gamepad.bRightTrigger > 64;
        if (lt != s.lt) { PushButton(c, 0x1BE, lt ? 1 : 0); s.lt = lt; }   // KEY_PAD_LTRIGGER
        if (rt != s.rt) { PushButton(c, 0x1BF, rt ? 1 : 0); s.rt = rt; }   // KEY_PAD_RTRIGGER
    }
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

    ss::Layout lay = ss::ComputeLayout((int)g_clients.size(), W, H, ss::TwoMode::Auto);
    bool drew = false;
    for (size_t i = 0; i < g_clients.size() && (int)i < lay.count; ++i) {
        OpenFb(g_clients[i]);
        const unsigned char* v = g_clients[i].view;
        if (!v) continue;
        const int* hdr = (const int*)v;
        if (hdr[0] != FB_MAGIC) continue;
        int fw = hdr[1], fh = hdr[2];
        if (fw <= 0 || fh <= 0) continue;
        ss::Rect box = ss::Letterbox(lay.panes[i], fw, fh);   // keep aspect (a no-op at native pane res)
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
    if (!drew) {   // nothing rendering yet -- show a loading message instead of a black window
        char msg[80];
        _snprintf(msg, sizeof(msg), "Starting %d-player splitscreen...  (%ld/%d)",
                  g_launch.n, (long)g_launchN, g_launch.n);
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

// The staggered launch (server + N clients, with the connect delays) runs on a background thread
// so the compositor window stays up + responsive the whole time, rather than appearing only after
// the last client. Clients are born hidden (DLL CreateWindowEx hook), so they never steal focus.
static DWORD WINAPI LaunchThread(LPVOID) {
    const char* dll = g_launch.dll; const char* exe = g_launch.exe;
    const char* iwad = g_launch.iwad; const char* cwd = g_launch.cwd; int n = g_launch.n;
    const char* seatsCfg = g_launch.seatsCfg;

    // Deploy the shared seat config to where every client +exec's it (splitseat.cfg in the game dir).
    // It's just the user-editable global_seats.cfg copied verbatim -- the config layer for all seats (binds,
    // cvars). If global_seats.cfg is missing, fall back to the essential controller-join binds so a stock
    // launch still works. (A multi-word bind can't ride the command line -- PerformBind keeps one
    // token and the cmdline drops the quotes -- but a cfg line preserves the quoted command.)
    {
        char cfgPath[600];
        _snprintf(cfgPath, sizeof(cfgPath), "%s\\splitseat.cfg", cwd);
        FILE* out = fopen(cfgPath, "w");
        if (out) {
            FILE* in = seatsCfg ? fopen(seatsCfg, "rb") : NULL;
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
    }

    char srvArgs[600];
    _snprintf(srvArgs, sizeof(srvArgs),
        "-host -iwad %s +map MAP01 +set sv_cooperative 1 +set sv_maxclients 8 +set sv_maxplayers 8 "
        "+set sv_maxclientsperip 8 +set sv_updatemaster 0 +set fullscreen 0", iwad);
    Launch srv = InjectSuspended(dll, exe, cwd, srvArgs);
    DWORD srvPid = ResumeAfterHooks(srv);
    if (srvPid) g_hidePids.push_back(srvPid);
    Sleep(1000);   // just enough for the server to bind its socket; -connect retries for the rest, so
                   // the clients' (slow) engine load overlaps the server's map load instead of waiting

    // Inject ALL clients suspended first (their DLL hook-installs then run concurrently), THEN resume
    // them -- so the per-client "wait for hooks" overlaps instead of serializing, and the engine loads
    // run in parallel. use_mouse 0: client never reads the physical mouse (we inject it). exec
    // splitseat.cfg: spare pad buttons -> "ifspectator menu_join" (controller spectator opens the JOIN
    // screen; a no-op once joined, via the engine's own commands -- no nag, no double-bind).
    // Render each client at ~its pane size when the window fills the monitor, times g_renderScale. The
    // compositor then HALFTONE-scales that into whatever the pane is actually displayed at, so the HUD
    // stays crisp at any window size / borderless / fullscreen / monitor -- no runtime mode switching.
    // render_scale < 1 renders a lower base and upscales (faster, softer); > 1 supersamples.
    int mw = (int)(GetSystemMetrics(SM_CXSCREEN) * g_renderScale);
    int mh = (int)(GetSystemMetrics(SM_CYSCREEN) * g_renderScale);
    if (mw < 640) mw = 640;  if (mh < 360) mh = 360;
    ss::Layout pl = ss::ComputeLayout(n, mw, mh, ss::TwoMode::Auto);
    Launch cl[4] = { 0 };
    for (int i = 0; i < n && i < 4; ++i) {
        int pw = (i < pl.count) ? pl.panes[i].w : 640;
        int ph = (i < pl.count) ? pl.panes[i].h : 360;
        if (pw > 1920) pw = 1920;  if (pw < 320) pw = 320;     // bound per-client render cost
        if (ph > 1080) ph = 1080;  if (ph < 180) ph = 180;
        // Every seat plays SFX (guns, menu blips, all gameplay) so you hear every player. Only seat 0
        // plays MUSIC -- N copies of the same track start out of sync and phase into a mess -- so the
        // other seats mute just the music (after the exec, so it overrides global_seats.cfg). The
        // one-shot connect sound is silenced separately via cl_connectsound 0 in global_seats.cfg.
        const char* audio = (i == 0) ? "" : "+set snd_musicvolume 0 ";
        char args[700];
        _snprintf(args, sizeof(args),
            "-iwad %s -connect 127.0.0.1 +set fullscreen 0 +set freelook 1 +set use_joystick 0 "
            "+set use_mouse 0 +exec splitseat.cfg %s"
            "+set vid_defwidth %d +set vid_defheight %d +name P%d", iwad, audio, pw, ph, i + 1);
        cl[i] = InjectSuspended(dll, exe, cwd, args);
    }
    for (int i = 0; i < n && i < 4; ++i) {
        DWORD pid = ResumeAfterHooks(cl[i]);
        if (pid) {
            Client c; c.pid = pid;
            CreateSeatIn(c);
            g_hidePids.push_back(pid);
            g_clients.push_back(c);   // published last; the render loop reads size() then [0..size-1]
        }
        InterlockedIncrement(&g_launchN);
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
    ss::Layout lay = ss::ComputeLayout((int)g_clients.size(), W, H, ss::TwoMode::Auto);
    for (size_t i = 0; i < g_clients.size() && (int)i < lay.count && i < 4; ++i) {
        OpenFb(g_clients[i]);
        const unsigned char* v = g_clients[i].view; if (!v) continue;
        const int* hdr = (const int*)v; if (hdr[0] != FB_MAGIC) continue;
        int fw = hdr[1], fh = hdr[2]; if (fw <= 0 || fh <= 0) continue;
        GTex& t = g_gtex[i];
        if (!t.tex || t.w != fw || t.h != fh) {         // (re)create this seat's texture at its render res
            if (t.srv) { t.srv->Release(); t.srv = nullptr; }
            if (t.tex) { t.tex->Release(); t.tex = nullptr; }
            D3D11_TEXTURE2D_DESC td = { 0 };
            td.Width = fw; td.Height = fh; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &t.tex))) continue;
            g_dev->CreateShaderResourceView(t.tex, nullptr, &t.srv);
            t.w = fw; t.h = fh;
        }
        D3D11_MAPPED_SUBRESOURCE ms;
        if (SUCCEEDED(g_ctx->Map(t.tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            const unsigned char* src = v + 16;          // BGRA, row pitch fw*4
            for (int y = 0; y < fh; ++y)
                memcpy((unsigned char*)ms.pData + (size_t)y * ms.RowPitch, src + (size_t)y * fw * 4, (size_t)fw * 4);
            g_ctx->Unmap(t.tex, 0);
        }
        ss::Rect box = ss::Letterbox(lay.panes[i], fw, fh);   // GPU bilinear-scales into the pane (crisp)
        D3D11_VIEWPORT vp = { (float)box.x, (float)box.y, (float)box.w, (float)box.h, 0.0f, 1.0f };
        g_ctx->RSSetViewports(1, &vp);
        g_ctx->PSSetShaderResources(0, 1, &t.srv);
        g_ctx->Draw(4, 0);
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

int main(int argc, char** argv) {
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
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "splitdronum", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) { if (L) { fprintf(L, "CreateWindow failed err=%lu\n", GetLastError()); fflush(L); } return 2; }
    SetForegroundWindow(hwnd);
    timeBeginPeriod(1);   // 1ms scheduler tick so the present loop runs uncapped, not ~64fps

    RAWINPUTDEVICE rid = { 0x01, 0x02, 0, hwnd };   // HID generic mouse -> WM_INPUT to this window
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
    ShowCursor(FALSE);

    // reserve so the background push_back never reallocates while the render loop reads g_clients
    g_clients.reserve(4);
    g_hidePids.reserve(8);
    g_launch.dll = dll; g_launch.exe = exe; g_launch.iwad = iwad; g_launch.cwd = cwd; g_launch.n = n;
    g_launch.seatsCfg = seatsCfg;
    g_hwnd = hwnd;
    CreateThread(NULL, 0, RenderThread, NULL, 0, NULL);   // compositing runs off the main thread
    CreateThread(NULL, 0, LaunchThread, NULL, 0, NULL);

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
                DWORD fgPid = 0; GetWindowThreadProcessId(fg, &fgPid);
                for (DWORD p : g_hidePids) if (p == fgPid) { SetForegroundWindow(hwnd); break; }
            }
            PollControllers(n);         // XInput pads -> seats 1..N-1
            // Hybrid mouse-free: release the mouse when seat 0 (the mouse user) is in a menu/console,
            // OR when every seat is (nobody is actively playing). The mouse is idle in that state
            // (m_use_mouse 0 means menus ignore it), so let it roam the desktop -- move the window, hit
            // the taskbar, alt-tab. The raw-input handler stops feeding seat 0 while freed (g_mouseFreed).
            bool freeMouse = false;
            size_t nc = g_clients.size();
            if (nc > 0) {
                bool seat0Menu = (g_clients[0].in && g_clients[0].in[IN_MENU] != 0);
                bool allMenu = true;
                for (size_t i = 0; i < nc; ++i)
                    if (!(g_clients[i].in && g_clients[i].in[IN_MENU] != 0)) { allMenu = false; break; }
                freeMouse = seat0Menu || allMenu;
            }
            if (freeMouse && !g_mouseFreed)      { g_mouseFreed = 1; ShowCursor(TRUE); }
            else if (!freeMouse && g_mouseFreed) { g_mouseFreed = 0; ShowCursor(FALSE); }
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
