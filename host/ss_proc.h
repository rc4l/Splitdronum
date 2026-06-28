// ss_proc -- cross-platform process launch + capture-library injection for the host.
//
// The host spawns stock Zandronum clients and injects the capture library that publishes each client's
// framebuffer/input over shared memory. Injection is the one genuinely platform-divergent piece:
//   Windows : launch suspended, LoadLibrary the DLL via CreateRemoteThread, wait for its window hooks to
//             arm (the "ZanHooks_<pid>" event), then resume -- no startup-window flash.
//   POSIX   : set DYLD_INSERT_LIBRARIES=<lib> in the child's environment; the dylib's constructor runs
//             before the engine's main(), so no suspend/resume dance is needed.
//
// The dedicated server is launched the same way but WITHOUT injection (injectLib == nullptr).
// Header-only. Runtime parity on macOS additionally requires the engine-side capture dylib to exist.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <spawn.h>
  #include <signal.h>
  #include <unistd.h>
  #include <sys/wait.h>
  #include <vector>
  #include <string>
  extern char** environ;
#endif

namespace ss {

#ifndef _WIN32
// Split a Windows-style single command-line string into argv, honoring double-quoted runs (profile names
// carry spaces, e.g. +name "angry imp"). Quotes are removed; whitespace outside quotes separates args.
inline void ProcTokenize(const char* s, std::vector<std::string>& out) {
    std::string cur; bool inq = false, has = false;
    for (const char* p = s; *p; ++p) {
        char ch = *p;
        if (ch == '"') { inq = !inq; has = true; }
        else if ((ch == ' ' || ch == '\t') && !inq) { if (has) { out.push_back(cur); cur.clear(); has = false; } }
        else { cur += ch; has = true; }
    }
    if (has) out.push_back(cur);
}
#endif

// Launch `exe` with command-line `args` in directory `cwd`. If injectLib != nullptr, inject it. Returns
// the child pid, or 0 on failure.
inline uint32_t ProcSpawn(const char* exe, const char* args, const char* cwd, const char* injectLib) {
#ifdef _WIN32
    char cmd[8192];
    _snprintf(cmd, sizeof(cmd), "\"%s\" %s", exe, args ? args : "");
    cmd[sizeof(cmd) - 1] = '\0';
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    DWORD flags = injectLib ? CREATE_SUSPENDED : 0;
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, flags, NULL, cwd, &si, &pi)) return 0;
    if (injectLib) {
        char ev[64]; _snprintf(ev, sizeof(ev), "ZanHooks_%lu", pi.dwProcessId);
        HANDLE hReady = CreateEventA(NULL, TRUE, FALSE, ev);   // the DLL sets this once its window hooks are live
        SIZE_T len = strlen(injectLib) + 1;
        void* mem = VirtualAllocEx(pi.hProcess, NULL, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        WriteProcessMemory(pi.hProcess, mem, injectLib, len, NULL);
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        HANDLE th = CreateRemoteThread(pi.hProcess, NULL, 0,
            (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryA"), mem, 0, NULL);
        if (th) { WaitForSingleObject(th, 10000); CloseHandle(th); }
        VirtualFreeEx(pi.hProcess, mem, 0, MEM_RELEASE);
        if (hReady) { WaitForSingleObject(hReady, 5000); CloseHandle(hReady); }   // wait for the window hooks
        ResumeThread(pi.hThread);
    }
    DWORD pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return pid;
#else
    std::vector<std::string> toks;
    toks.push_back(exe);
    if (args) ProcTokenize(args, toks);
    std::vector<char*> argv;
    for (auto& t : toks) argv.push_back(const_cast<char*>(t.c_str()));
    argv.push_back(nullptr);

    // child environment: inherit ours, plus DYLD_INSERT_LIBRARIES=<lib> when injecting
    std::vector<std::string> envstore;
    std::vector<char*> envp;
    std::string inject;
    if (injectLib) { inject = std::string("DYLD_INSERT_LIBRARIES=") + injectLib; }
    for (char** e = environ; e && *e; ++e) {
        if (injectLib && strncmp(*e, "DYLD_INSERT_LIBRARIES=", 22) == 0) continue;   // we replace it
        envp.push_back(*e);
    }
    if (injectLib) envp.push_back(const_cast<char*>(inject.c_str()));
    envp.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (cwd) posix_spawn_file_actions_addchdir_np(&fa, cwd);   // run the child in the gamedir
    pid_t pid = 0;
    int rc = posix_spawn(&pid, exe, &fa, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    return rc == 0 ? (uint32_t)pid : 0;
#endif
}

// True if the process is still running.
inline bool ProcAlive(uint32_t pid) {
    if (!pid) return false;
#ifdef _WIN32
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return false;
    bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
#else
    return kill((pid_t)pid, 0) == 0;
#endif
}

// Force-stop immediately.
inline void ProcKillForce(uint32_t pid) {
    if (!pid) return;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (h) { TerminateProcess(h, 0); CloseHandle(h); }
#else
    kill((pid_t)pid, SIGKILL);
#endif
}

#ifdef _WIN32
inline BOOL CALLBACK ProcCloseByPid_(HWND h, LPARAM pid) {
    DWORD wp = 0; GetWindowThreadProcessId(h, &wp);
    if (wp == (DWORD)pid) PostMessageA(h, WM_CLOSE, 0, 0);
    return TRUE;
}
#endif

// Ask the process to quit CLEANLY (so the engine saves its per-seat ini and disconnects), wait briefly,
// then force-kill if it hangs. Windows posts WM_CLOSE to its windows; POSIX sends SIGTERM.
inline void ProcKillGraceful(uint32_t pid) {
    if (!pid) return;
#ifdef _WIN32
    EnumWindows(ProcCloseByPid_, (LPARAM)pid);
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
    if (h) {
        if (WaitForSingleObject(h, 4000) != WAIT_OBJECT_0) TerminateProcess(h, 0);
        CloseHandle(h);
    }
#else
    kill((pid_t)pid, SIGTERM);
    for (int i = 0; i < 40 && ProcAlive(pid); ++i) usleep(100 * 1000);   // up to ~4s for a clean exit
    if (ProcAlive(pid)) kill((pid_t)pid, SIGKILL);
    int st = 0; waitpid((pid_t)pid, &st, WNOHANG);   // reap if it's our child
#endif
}

}  // namespace ss
