// ss_hook -- the injected DLL (Phase 2 de-risk). Hooks wglSwapBuffers on a STOCK,
// unmodified Zandronum/Doom client and, each frame, reads the GL back buffer and
// publishes it to a named shared section. This is the runtime-hook replacement for the
// old hs_fbshare engine source-edit -- no engine changes, the binary stays pristine.
//
// Verify: the host opens "ZanDLLFB_<pid>" and sees an advancing, non-black framebuffer.
#include <windows.h>
#include <timeapi.h>   // timeBeginPeriod -- 1ms Sleep granularity for the frame-rate cap
#include <GL/gl.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include "MinHook.h"
#include "ss_offsets.h"   // generated at build time: SS_RVA_<name> for each engine symbol

#ifndef GL_BGR_EXT
#define GL_BGR_EXT 0x80E0
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif

namespace {

typedef BOOL(WINAPI* SwapBuffers_t)(HDC);
SwapBuffers_t  g_realSwap = nullptr;
HANDLE         g_map = nullptr;
unsigned char* g_view = nullptr;
size_t         g_size = 0;
int            g_frame = 0;
const int      FB_MAGIC = 0x5A444C46;  // 'ZDLF'

// Hide every window this process tries to show, so only the compositor is ever visible (no
// startup/console/game-window flash on either the clients or the server). The window still has a
// valid DC, so the GL context + rendering work fine while hidden.
typedef BOOL(WINAPI* ShowWindow_t)(HWND, int);
ShowWindow_t   g_realShowWindow = nullptr;
BOOL WINAPI HookedShowWindow(HWND hwnd, int) {
    return g_realShowWindow(hwnd, SW_HIDE);
}

// The engine creates its main window WS_VISIBLE (so the ShowWindow hook alone misses it) and
// RestoreConView re-shows it, so a small thread just keeps this process's own top-level windows
// hidden. From inside the process this lands within a couple ms of the window appearing -- no
// visible startup flash on either the clients or the server.
BOOL CALLBACK HideEnumProc(HWND h, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h)) ShowWindow(h, SW_HIDE);
    return TRUE;
}
DWORD WINAPI HideThread(LPVOID) {
    for (;;) { EnumWindows(HideEnumProc, 0); Sleep(4); }
}

// Strip WS_VISIBLE from every window the engine creates, so its window is born hidden -- no flash
// and, crucially, no stealing focus from the compositor during the staggered startup.
typedef HWND(WINAPI* CreateWindowExA_t)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
typedef HWND(WINAPI* CreateWindowExW_t)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
CreateWindowExA_t g_realCWExA = nullptr;
CreateWindowExW_t g_realCWExW = nullptr;
HWND WINAPI HookedCWExA(DWORD ex, LPCSTR c, LPCSTR n, DWORD s, int x, int y, int w, int h, HWND p, HMENU m, HINSTANCE i, LPVOID l) {
    return g_realCWExA(ex, c, n, s & ~WS_VISIBLE, x, y, w, h, p, m, i, l);
}
HWND WINAPI HookedCWExW(DWORD ex, LPCWSTR c, LPCWSTR n, DWORD s, int x, int y, int w, int h, HWND p, HMENU m, HINSTANCE i, LPVOID l) {
    return g_realCWExW(ex, c, n, s & ~WS_VISIBLE, x, y, w, h, p, m, i, l);
}

// RestoreConView() re-shows the startup window during the load by SETTING its style to WS_VISIBLE
// via SetWindowLongPtr -- invisible to both the CreateWindowEx and ShowWindow hooks. Strip
// WS_VISIBLE off any GWL_STYLE write so the window stays hidden (this is what was flashing).
typedef LONG_PTR(WINAPI* SetWLP_t)(HWND, int, LONG_PTR);
SetWLP_t g_realSetWLPA = nullptr;
SetWLP_t g_realSetWLPW = nullptr;
LONG_PTR WINAPI HookedSetWLPA(HWND h, int idx, LONG_PTR v) {
    if (idx == GWL_STYLE) v &= ~(LONG_PTR)WS_VISIBLE;
    return g_realSetWLPA(h, idx, v);
}
LONG_PTR WINAPI HookedSetWLPW(HWND h, int idx, LONG_PTR v) {
    if (idx == GWL_STYLE) v &= ~(LONG_PTR)WS_VISIBLE;
    return g_realSetWLPW(h, idx, v);
}
// The GL window setup uses the 32-bit SetWindowLong (win32gliface.cpp), a distinct function from
// SetWindowLongPtr on x64 -- hook it the same way so the game window also stays hidden.
typedef LONG(WINAPI* SetWL_t)(HWND, int, LONG);
SetWL_t g_realSetWLA = nullptr;
SetWL_t g_realSetWLW = nullptr;
LONG WINAPI HookedSetWLA(HWND h, int idx, LONG v) {
    if (idx == GWL_STYLE) v &= ~(LONG)WS_VISIBLE;
    return g_realSetWLA(h, idx, v);
}
LONG WINAPI HookedSetWLW(HWND h, int idx, LONG v) {
    if (idx == GWL_STYLE) v &= ~(LONG)WS_VISIBLE;
    return g_realSetWLW(h, idx, v);
}

// We drive input by posting the engine's own events (resolved D_PostEvent), so the engine's
// existing pipeline does the work -- mouse_sensitivity / m_yaw / m_pitch / invertmouse, and
// the game-vs-menu routing -- using each client's own config. No look math or gating here.
// event_t mirrors d_event.h at default alignment: type@0 subtype@1 data1/2/3@2/4/6 x@8 y@12.
struct EngEvent { unsigned char type, subtype; short data1, data2, data3; int x, y; };
typedef void(*PostEvent_t)(const EngEvent*);
PostEvent_t g_postEvent = nullptr;
// AddCommandString(text, keynum=0): run a console command line. Drives the host's runtime command channel
// (handing music ownership to another seat when seat 0 quits). Benign cvar/exec only -- never vid_setmode.
typedef void(*AddCmd_t)(char*, int);
AddCmd_t g_addCmd = nullptr;
LONG     g_cmdSeq = 0;
// d_event.h EGenericEvent + d_gui.h EGUIEvent subtypes -- the engine's event protocol (stable).
enum { EV_KeyDown = 1, EV_KeyUp = 2, EV_Mouse = 3, EV_GUI_Event = 4 };
enum { GUI_KeyDown = 1, GUI_KeyUp = 3, GUI_Char = 4,        // d_gui.h EGUIEvent subtypes
       GUI_MouseMove = 6, GUI_LBtnDown = 7, GUI_LBtnUp = 8,
       GUI_MBtnDown = 10, GUI_MBtnUp = 11, GUI_RBtnDown = 13, GUI_RBtnUp = 14 };
enum { SC_GRAVE = 0x29, KEY_MOUSE1 = 0x100, KEY_MOUSE3 = 0x102 };  // grave=console toggle; mouse-btn scancodes
// Engine global: 0 = MENU_Off (gameplay); nonzero = a menu is up. We route keyboard input by
// this -- UI keys go as EV_GUI, gameplay keys as EV_KeyDown -- so a menu key never collides
// with the open/close toggle and gameplay binds never bleed into an open menu.
int* g_menuActive = nullptr;
// Engine console state (c_up=0, c_down=1, c_falling=2, c_rising=3). The console takes GUI input
// when down/falling (C_Responder's own condition), so we route text to it just like a menu.
int* g_consoleState = nullptr;
// Engine global: nonzero whenever the GUI owns the keyboard -- a menu, the console, OR chat
// (messagemode) and any other text field. The single source of truth the native input layer uses to
// decide GUI-vs-gameplay routing; reading it directly is what makes chat/say work (chat is neither a
// menu nor the console, so the menu+console reconstruction missed it).
unsigned char* g_guiCapture = nullptr;
// Zandronum's chat (messagemode/say) is its OWN system, separate from ZDoom's GUICapture: opening chat
// only sets g_ulChatMode (CHATMODE_NONE=0 when not chatting). CHAT_Input consumes EV_GUI_Events, so we
// must route input as GUI while this is nonzero -- otherwise 't' opens chat but you can't type/send/escape.
unsigned int* g_chatMode = nullptr;
// Virtual GUI cursor for menus/console: use_mouse 0 kills the engine's native cursor, so we feed
// EV_GUI_MouseMove ourselves, driving an absolute cursor with the same per-frame mouse deltas.
int  g_vpW = 640, g_vpH = 360;     // current render size, refreshed each frame in HookedSwap
int  g_curX = 320, g_curY = 180;   // cursor position in screen pixels
bool g_guiWas = false;             // was the GUI active last frame (to recenter the cursor on open)
bool g_invMouseY = false;          // mirror the host's SS_MOUSE_INVY so the cursor tracks naturally
// Engine global "is this the active app". We hold it true so a backgrounded client keeps
// rendering and never idles -- the runtime replacement for the vid_renderwhileinactive edit.
volatile int* g_appActive = nullptr;

// I_GetAxes(float[NUM_JOYAXIS]) -- hooked so the engine reads THIS client's controller axes from
// shared memory (native analog movement + joystick look via G_BuildTiccmd) instead of the
// globally-shared physical device. m_joy.h order: Yaw, Pitch, Forward, Side, Up.
typedef void(*GetAxes_t)(float*);
GetAxes_t  g_realGetAxes = nullptr;
const int  NUM_JOYAXIS = 5;
const int  IN_AXES    = 136;   // ZanIN int index of the 5 fixed-point axes (host writes value x1000)
const int  IN_MENU    = 160;   // ZanIN int the DLL writes BACK: 1 = this seat is in a menu/console
const int  IN_CMDSEQ  = 164;   // host bumps this after writing a console command at IN_CMDTEXT
const int  IN_CMDTEXT = 168;   // the command string (bytes), up to IN_CMDMAX
const int  IN_CMDMAX  = 120;

// Host -> client input: shared section "ZanIN_<pid>" (1 KB of ints). The host writes raw input;
// the DLL forwards it as engine events and lets the engine do binds / menu / look.
//   [0]=magic 'ZANI'   [3]=mouse dx   [4]=mouse dy   (deltas, consumed each frame)
//   [5]=key write-count;  ring of 64 events at [8], 2 ints each:
//     a = (scancode<<1)|down ;   b = gk | (char<<16)
// Per event the DLL posts EV_KeyDown/Up (scancode -> binds: move/weapons/fire),
// EV_GUI_KeyDown/Up (gk -> menu/console), and EV_GUI_Char (char -> text). Mouse buttons arrive
// as key events with KEY_MOUSE* scancodes -- no special-casing here.
const int      IN_MAGIC = 0x5A414E49;
const int      IN_BYTES = 1024;
const int      KEY_RING = 64;
HANDLE         g_inMap = nullptr;
volatile LONG* g_in = nullptr;
int            g_keyRead = 0;

void ApplyInput() {
    if (g_in == nullptr) {
        char nm[64];
        _snprintf(nm, sizeof(nm), "ZanIN_%lu", GetCurrentProcessId());
        nm[sizeof(nm) - 1] = '\0';
        g_inMap = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, nm);
        if (g_inMap == nullptr) return;                       // host hasn't created it yet
        g_in = (volatile LONG*)MapViewOfFile(g_inMap, FILE_MAP_ALL_ACCESS, 0, 0, IN_BYTES);
        if (g_in == nullptr) { CloseHandle(g_inMap); g_inMap = nullptr; return; }
        g_keyRead = g_in[5];                                  // start at the host's write head
        g_cmdSeq  = g_in[IN_CMDSEQ];                           // don't replay a command issued before we mapped
    }
    if (g_in[0] != IN_MAGIC || !g_postEvent) return;

    // Runtime command channel: the host bumps IN_CMDSEQ after writing a console command at IN_CMDTEXT
    // (e.g. handing music ownership to this seat when seat 0 quits). Run each new command once. Only
    // benign cvar/exec commands are sent -- nothing that tears down the GL context/window.
    if (g_addCmd) {
        LONG seq = g_in[IN_CMDSEQ];
        if (seq != g_cmdSeq) {
            g_cmdSeq = seq;
            char cmd[IN_CMDMAX];
            memcpy(cmd, (const void*)(g_in + IN_CMDTEXT), IN_CMDMAX);
            cmd[IN_CMDMAX - 1] = '\0';
            if (cmd[0]) g_addCmd(cmd, 0);
        }
    }

    // GUI = a menu, OR the console while it's down/falling (C_Responder's own condition). When the
    // GUI is up it takes the keyboard, the mouse buttons (as clicks), and an absolute cursor;
    // otherwise input drives gameplay (look + binds). This is the same gating the native engine does.
    bool menuOpen = (g_menuActive && *g_menuActive != 0);
    int  cs = (g_consoleState ? *g_consoleState : 0);
    // GUICapture = ZDoom GUI (menu/console/etc.); g_chatMode = Zandronum chat. Either means the keyboard
    // belongs to a text UI. (menu+console reads kept as a belt-and-suspenders fallback.)
    bool guiOpen = (g_guiCapture && *g_guiCapture != 0) || (g_chatMode && *g_chatMode != 0)
                 || menuOpen || cs == 1 || cs == 2;
    if (guiOpen && !g_guiWas) { g_curX = g_vpW / 2; g_curY = g_vpH / 2; }   // recenter on open
    g_guiWas = guiOpen;
    g_in[IN_MENU] = guiOpen ? 1 : 0;   // publish to the host: this seat is in a menu OR the console --
                                       // both take the mouse off gameplay, so free it for the desktop.
                                       // (The brief startup connect-log console frees it too, harmlessly
                                       // -- the game isn't playable yet anyway.)

    int dx = (int)InterlockedExchange(&g_in[3], 0);           // consume raw per-frame deltas
    int dy = (int)InterlockedExchange(&g_in[4], 0);           // (host already applied look-invert to dy)
    if (dx || dy) {
        if (guiOpen) {                                        // drive the GUI cursor (absolute pixels)
            int rawdy = g_invMouseY ? dy : -dy;               // undo host look-invert: mouse-down = +y
            g_curX += dx;    if (g_curX < 0) g_curX = 0; if (g_curX > g_vpW) g_curX = g_vpW;
            g_curY += rawdy; if (g_curY < 0) g_curY = 0; if (g_curY > g_vpH) g_curY = g_vpH;
            EngEvent ev = { 0 };
            ev.type = EV_GUI_Event; ev.subtype = GUI_MouseMove;
            ev.data1 = (short)g_curX; ev.data2 = (short)g_curY;   // menu reads position from data1/data2
            g_postEvent(&ev);
        } else {                                              // look drives gameplay
            EngEvent ev = { 0 };
            // Match the engine's native mouse prescale: i_mouse.cpp does lLastX << 2 (x4) on X unless
            // m_noprescale. We bypass that path with use_mouse 0, so without this X look is 1/4 speed.
            ev.type = EV_Mouse; ev.x = dx << 2; ev.y = dy;
            g_postEvent(&ev);                                 // engine converts + routes it
        }
    }

    int keyWrite = g_in[5];
    if (keyWrite - g_keyRead > KEY_RING) g_keyRead = keyWrite - KEY_RING;   // skip overwritten
    for (int guard = 0; g_keyRead != keyWrite && guard < KEY_RING; ++guard, ++g_keyRead) {
        int base = 8 + (g_keyRead % KEY_RING) * 2;
        int a = g_in[base], b = g_in[base + 1];
        int down = a & 1, sc = (a >> 1) & 0x7fff, gk = b & 0xffff, ch = (b >> 16) & 0xffff;
        int raw = (a >> 16) & 1;
        bool mouseBtn = (sc >= KEY_MOUSE1 && sc <= KEY_MOUSE3);
        if (raw) {                                            // controller button (KEY_PAD_*): the
            EngEvent ev = { 0 };                              // engine routes it itself (game/menu), so
            ev.type = (unsigned char)(down ? EV_KeyDown : EV_KeyUp);  // it's always raw EV_KeyDown
            ev.data1 = (short)sc;
            g_postEvent(&ev);
        } else if (sc == SC_GRAVE) {                          // console toggle is an EV_KeyDown bind --
            EngEvent ev = { 0 };                              // always raw so tilde OPENS and CLOSES it
            ev.type = (unsigned char)(down ? EV_KeyDown : EV_KeyUp);
            ev.data1 = (short)sc;
            g_postEvent(&ev);
        } else if (guiOpen) {                                 // keyboard/mouse drive the UI
            if (mouseBtn) {                                   // click menu items at the cursor
                int dn = (sc == KEY_MOUSE1) ? GUI_LBtnDown : (sc == KEY_MOUSE3) ? GUI_MBtnDown : GUI_RBtnDown;
                int up = (sc == KEY_MOUSE1) ? GUI_LBtnUp   : (sc == KEY_MOUSE3) ? GUI_MBtnUp   : GUI_RBtnUp;
                EngEvent ev = { 0 };
                ev.type = EV_GUI_Event; ev.subtype = (unsigned char)(down ? dn : up);
                ev.data1 = (short)g_curX; ev.data2 = (short)g_curY;   // click position in data1/data2
                g_postEvent(&ev);
            } else {
                if (gk) {                                     // nav (arrows/enter/escape-close)
                    EngEvent ev = { 0 };
                    ev.type = EV_GUI_Event;
                    ev.subtype = (unsigned char)(down ? GUI_KeyDown : GUI_KeyUp);
                    ev.data1 = (short)gk; ev.data2 = (short)gk;
                    g_postEvent(&ev);
                }
                if (ch && down) {                             // text entry (console / chat / name fields)
                    EngEvent ev = { 0 };
                    ev.type = EV_GUI_Event; ev.subtype = GUI_Char;
                    ev.data1 = (short)ch;
                    g_postEvent(&ev);
                }
            }
        } else if (sc) {                                      // gameplay binds: move/weapons/fire
            EngEvent ev = { 0 };                              //   (incl. KEY_MOUSE1; EV_KeyDown(ESC) opens menu)
            ev.type = (unsigned char)(down ? EV_KeyDown : EV_KeyUp);
            ev.data1 = (short)sc; ev.data2 = (short)ch;
            g_postEvent(&ev);
        }
    }
}

// Replaces the engine's I_GetAxes: fill the axis array from this seat's ZanIN instead of the
// physical device. We do NOT call the original (that read the shared device -> every seat moved).
void HookedGetAxes(float* axes) {
    for (int i = 0; i < NUM_JOYAXIS; ++i) axes[i] = 0.f;
    if (g_in && g_in[0] == IN_MAGIC)
        for (int i = 0; i < NUM_JOYAXIS; ++i) axes[i] = (float)g_in[IN_AXES + i] / 1000.0f;
}

void Publish(int w, int h) {
    size_t need = 16 + (size_t)w * h * 4;   // BGRA (32-bit) so the host can upload straight to a GPU texture
    if (g_map == nullptr || g_size < need) {
        if (g_view) { UnmapViewOfFile(g_view); g_view = nullptr; }
        if (g_map)  { CloseHandle(g_map);       g_map = nullptr; }
        char name[64];
        _snprintf(name, sizeof(name), "ZanDLLFB_%lu", GetCurrentProcessId());
        name[sizeof(name) - 1] = '\0';
        g_map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD)need, name);
        if (g_map == nullptr) return;
        g_view = (unsigned char*)MapViewOfFile(g_map, FILE_MAP_WRITE, 0, 0, need);
        if (g_view == nullptr) { CloseHandle(g_map); g_map = nullptr; return; }
        g_size = need;
    }
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, g_view + 16);  // BGRA, bottom-up
    int* hdr = (int*)g_view;
    hdr[1] = w; hdr[2] = h; hdr[3] = ++g_frame;
    hdr[0] = FB_MAGIC;  // written last so a reader never sees a torn header
}

BOOL WINAPI HookedSwap(HDC hdc) {
    // Resolve the caps once. RENDER cap = SS_FPS (play.ps1 -Fps): a number, 0 = uncapped, or
    // unset/"auto" = the monitor's refresh rate. The engine's own vid_maxfps is bypassed for our
    // hidden force-active clients (uncapped they free-run at ~2000fps and saturate the GPU), and the
    // mouse is injected once per render frame, so this also sets seat 0's input latency. READBACK cap
    // = the refresh rate: the compositor presents at the display rate, so a faster GPU->CPU copy of
    // the framebuffer is pure waste (and that readback is the dominant cost at high res).
    static int s_renderCap = -2, s_readCap = 60;
    if (s_renderCap == -2) {
        int refresh = 60;
        DEVMODE dm = { 0 }; dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency >= 24)
            refresh = (int)dm.dmDisplayFrequency;
        s_readCap = refresh;
        const char* e = getenv("SS_FPS");
        s_renderCap = (e && *e && strcmp(e, "auto") != 0) ? atoi(e) : refresh;
    }
    if (s_renderCap > 0) {
        static LARGE_INTEGER freq = { 0 }, last = { 0 };
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        const double targetMs = 1000.0 / (double)s_renderCap;
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        if (last.QuadPart) {
            double dt = (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)freq.QuadPart;
            if (dt < targetMs) { Sleep((DWORD)(targetMs - dt)); QueryPerformanceCounter(&now); }
        }
        last = now;
    }
    GLint vp[4] = { 0, 0, 0, 0 };
    glGetIntegerv(GL_VIEWPORT, vp);
    int w = vp[2], h = vp[3];
    if (w > 0 && h > 0 && w <= 8192 && h <= 8192) {
        g_vpW = w; g_vpH = h;   // viewport feeds the GUI cursor + input -- update every frame
        static LARGE_INTEGER pf = { 0 }, pl = { 0 };
        if (!pf.QuadPart) QueryPerformanceFrequency(&pf);
        LARGE_INTEGER pn; QueryPerformanceCounter(&pn);
        double pdt = pl.QuadPart ? (double)(pn.QuadPart - pl.QuadPart) * 1000.0 / (double)pf.QuadPart : 1e9;
        if (s_readCap <= 0 || pdt >= 1000.0 / (double)s_readCap) { Publish(w, h); pl = pn; }
    }
    ApplyInput();   // read the host's input from shared memory and drive the engine
    return g_realSwap(hdc);
}

DWORD WINAPI InitThread(LPVOID) {
    timeBeginPeriod(1);   // 1ms timer so the swap-hook frame cap can pace precisely
    CreateThread(NULL, 0, HideThread, NULL, 0, NULL);   // hide our windows ASAP, continuously
    if (MH_Initialize() != MH_OK) return 0;

    // Hook ShowWindow FIRST (before the engine creates its window) so nothing ever flashes -- on
    // the clients AND the dedicated server (which has no GL, so it falls through the rest).
    HMODULE u32 = GetModuleHandleA("user32.dll");
    void* swTarget = (void*)GetProcAddress(u32, "ShowWindow");
    if (swTarget && MH_CreateHook(swTarget, (void*)&HookedShowWindow, (void**)&g_realShowWindow) == MH_OK)
        MH_EnableHook(swTarget);
    void* cwa = (void*)GetProcAddress(u32, "CreateWindowExA");
    if (cwa && MH_CreateHook(cwa, (void*)&HookedCWExA, (void**)&g_realCWExA) == MH_OK) MH_EnableHook(cwa);
    void* cww = (void*)GetProcAddress(u32, "CreateWindowExW");
    if (cww && MH_CreateHook(cww, (void*)&HookedCWExW, (void**)&g_realCWExW) == MH_OK) MH_EnableHook(cww);
    void* swlpA = (void*)GetProcAddress(u32, "SetWindowLongPtrA");
    if (swlpA && MH_CreateHook(swlpA, (void*)&HookedSetWLPA, (void**)&g_realSetWLPA) == MH_OK) MH_EnableHook(swlpA);
    void* swlpW = (void*)GetProcAddress(u32, "SetWindowLongPtrW");
    if (swlpW && MH_CreateHook(swlpW, (void*)&HookedSetWLPW, (void**)&g_realSetWLPW) == MH_OK) MH_EnableHook(swlpW);
    void* swlA = (void*)GetProcAddress(u32, "SetWindowLongA");
    if (swlA && MH_CreateHook(swlA, (void*)&HookedSetWLA, (void**)&g_realSetWLA) == MH_OK) MH_EnableHook(swlA);
    void* swlW = (void*)GetProcAddress(u32, "SetWindowLongW");
    if (swlW && MH_CreateHook(swlW, (void*)&HookedSetWLW, (void**)&g_realSetWLW) == MH_OK) MH_EnableHook(swlW);

    // Signal the injector that the window hooks are live, so it can resume the engine's main thread.
    // Until now the main thread is suspended, so the engine can't create its startup window (born
    // with WS_VISIBLE) before the hook strips it -- that race is what flashed the window.
    {
        char ev[64]; _snprintf(ev, sizeof(ev), "ZanHooks_%lu", GetCurrentProcessId()); ev[63] = '\0';
        HANDLE h = OpenEventA(EVENT_MODIFY_STATE, FALSE, ev);
        if (h) { SetEvent(h); CloseHandle(h); }
    }

    // Hook wglSwapBuffers for the framebuffer once the GL ICD is loaded (clients render; a
    // dedicated server never loads opengl32, so just skip it there).
    HMODULE gl = nullptr;
    for (int i = 0; i < 200 && gl == nullptr; ++i) {
        gl = GetModuleHandleA("opengl32.dll");
        if (gl == nullptr) Sleep(25);
    }
    if (gl) {
        void* target = (void*)GetProcAddress(gl, "wglSwapBuffers");
        if (target && MH_CreateHook(target, (void*)&HookedSwap, (void**)&g_realSwap) == MH_OK)
            MH_EnableHook(target);
    }

    // Resolve engine symbols as (exe module base + build-time RVA from ss_offsets.h). No runtime
    // DbgHelp -- deterministic and instant. (Runtime DbgHelp raced the engine's own DbgHelp use
    // and resolved only intermittently.) To add a symbol: extend the gen_offsets call in
    // build-dll.ps1 and reference its SS_RVA_<name> here -- a missing one fails the BUILD.
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);   // the zandronum.exe main module
    g_postEvent    = (PostEvent_t)(base + SS_RVA_D_PostEvent);
    g_menuActive   = (int*)(base + SS_RVA_menuactive);
    g_consoleState = (int*)(base + SS_RVA_ConsoleState);
    g_guiCapture   = (unsigned char*)(base + SS_RVA_GUICapture);
    g_chatMode     = (unsigned int*)(base + SS_RVA_g_ulChatMode);
    g_appActive    = (volatile int*)(base + SS_RVA_AppActive);
    g_addCmd       = (AddCmd_t)(base + SS_RVA_AddCommandString);   // runtime console command channel
    g_invMouseY    = (getenv("SS_MOUSE_INVY") != nullptr);   // match the host so the GUI cursor tracks naturally

    // Hook I_GetAxes so the engine reads this seat's controller axes from shared memory -- native
    // analog movement + joystick look, per client, with no shared-device leak. (MinHook was
    // already initialized for wglSwapBuffers above.)
    void* axTarget = (void*)(base + SS_RVA_I_GetAxes);
    if (MH_CreateHook(axTarget, (void*)&HookedGetAxes, (void**)&g_realGetAxes) == MH_OK)
        MH_EnableHook(axTarget);

    // Hold the engine's active flag true so a backgrounded, hidden client keeps rendering (so the
    // compositor always has a fresh frame) -- replaces vid_renderwhileinactive. The thread never
    // returns; that's fine, it IS the keep-alive.
    for (;;) { *g_appActive = 1; Sleep(8); }
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
    }
    return TRUE;
}
