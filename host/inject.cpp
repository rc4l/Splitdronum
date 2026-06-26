// inject -- launch a stock client with our DLL injected (CreateProcess suspended ->
// LoadLibrary in the target via a remote thread -> resume). Phase-2 de-risk launcher;
// the real host will do this for N clients and composite them.
//
//   inject.exe <full\path\ss_hook.dll> <full\path\game.exe> [game args...]
#include <windows.h>
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("usage: inject <dll> <exe> [args...]\n");
        return 1;
    }
    const char* dll = argv[1];
    const char* exe = argv[2];

    // Build a command line: "exe" "arg1" "arg2" ...
    char cmd[8192] = { 0 };
    for (int i = 2; i < argc; ++i) {
        strcat_s(cmd, sizeof(cmd), "\"");
        strcat_s(cmd, sizeof(cmd), argv[i]);
        strcat_s(cmd, sizeof(cmd), "\" ");
    }

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        printf("CreateProcess failed: %lu\n", GetLastError());
        return 1;
    }

    // write the DLL path into the target and run LoadLibraryA there
    SIZE_T len = strlen(dll) + 1;
    void* mem = VirtualAllocEx(pi.hProcess, NULL, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (mem == NULL) { printf("VirtualAllocEx failed: %lu\n", GetLastError()); return 1; }
    WriteProcessMemory(pi.hProcess, mem, dll, len, NULL);

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    LPTHREAD_START_ROUTINE loadLib = (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryA");
    HANDLE th = CreateRemoteThread(pi.hProcess, NULL, 0, loadLib, mem, 0, NULL);
    if (th == NULL) { printf("CreateRemoteThread failed: %lu\n", GetLastError()); return 1; }
    WaitForSingleObject(th, 10000);
    DWORD loaded = 0;
    GetExitCodeThread(th, &loaded);   // nonzero (the HMODULE) means LoadLibrary succeeded
    CloseHandle(th);
    VirtualFreeEx(pi.hProcess, mem, 0, MEM_RELEASE);

    ResumeThread(pi.hThread);
    printf("pid=%lu injected=%s\n", pi.dwProcessId, loaded ? "yes" : "no");
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return loaded ? 0 : 2;
}
