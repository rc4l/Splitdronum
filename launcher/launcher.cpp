// splitdronum launcher -- a thin, cross-platform GUI over play.ps1's parameters.
//
// Dear ImGui (GLFW + OpenGL3). The launcher itself is pure portable C++ and builds on
// Windows/Linux/macOS; the *launch action* shells out to play.ps1 and is Windows-only (so is the
// splitdronum runtime: DLL injection + WGL + the D3D11 compositor). It's guarded behind _WIN32 --
// on other OSes the UI runs but Play reports the runtime isn't ported there yet.
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#include <cstdio>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

static char g_status[512] = "";

// repo root = where play.ps1 lives, found relative to this exe (built into <repo>/build alongside host.exe)
static std::string RepoRoot() {
#ifdef _WIN32
    char p[MAX_PATH] = { 0 };
    GetModuleFileNameA(NULL, p, MAX_PATH);
    std::string s(p);
    size_t a = s.find_last_of("\\/");              // strip the exe name -> ...\build
    if (a != std::string::npos) s = s.substr(0, a);
    size_t b = s.find_last_of("\\/");              // strip one more -> the repo root
    if (b != std::string::npos) s = s.substr(0, b);
    return s;
#else
    return ".";
#endif
}

// Build the play.ps1 invocation from the form state and run it detached.
static void Launch(int players, const char* iwad, const char* gamePath, float scale,
                   int fpsMode, int fpsCustom) {
#ifdef _WIN32
    std::string root = RepoRoot();
    int fps = (fpsMode == 0) ? -1 : (fpsMode == 1) ? 0 : fpsCustom;   // refresh / uncapped / custom
    char num[64];
    std::string a = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + root + "\\play.ps1\"";
    _snprintf(num, sizeof(num), " -Players %d", players);        a += num;
    a += std::string(" -Iwad \"") + iwad + "\"";
    _snprintf(num, sizeof(num), " -RenderScale %.2f", scale);    a += num;
    _snprintf(num, sizeof(num), " -Fps %d", fps);                a += num;
    if (gamePath && gamePath[0]) a += std::string(" -GamePath \"") + gamePath + "\"";
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    std::vector<char> buf(a.begin(), a.end()); buf.push_back('\0');
    if (CreateProcessA(NULL, buf.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, root.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        _snprintf(g_status, sizeof(g_status), "Launched %d-player session. (Close the game window to stop.)", players);
    } else {
        _snprintf(g_status, sizeof(g_status), "Could not start play.ps1 (error %lu). Is play.ps1 in the repo root?", GetLastError());
    }
#else
    (void)players; (void)iwad; (void)gamePath; (void)scale; (void)fpsMode; (void)fpsCustom;
    _snprintf(g_status, sizeof(g_status),
              "The splitdronum runtime is Windows-only for now -- launching isn't available on this OS yet.");
#endif
}

static void glfw_error(int e, const char* d) { fprintf(stderr, "GLFW %d: %s\n", e, d); }

int main(int, char**) {
    glfwSetErrorCallback(glfw_error);
    if (!glfwInit()) return 1;
    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    GLFWwindow* window = glfwCreateWindow(720, 450, "splitdronum", NULL, NULL);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);   // vsync -- a launcher needn't spin the CPU

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = NULL;   // don't litter an imgui.ini next to the exe
    ImGui::StyleColorsDark();
    ImGui::GetStyle().FrameRounding = 4.0f;
    ImGui::GetStyle().WindowPadding = ImVec2(16, 14);
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // form state (defaults mirror play.ps1)
    int   players = 2;
    char  iwad[128] = "freedoom2.wad";
    char  gamePath[512] = "";          // blank -> play.ps1 uses the sibling zandronum build
    float scale = 1.0f;
    int   fpsMode = 0;                 // 0 = monitor refresh, 1 = uncapped, 2 = custom
    int   fpsCustom = 240;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("splitdronum", NULL,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBringToFrontOnFocus);

        ImGui::Text("splitdronum");
        ImGui::TextDisabled("Local-splitscreen co-op for Zandronum  -  one window, N stock clients");
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::PushItemWidth(300);
        ImGui::SliderInt("Players", &players, 1, 4);
        ImGui::SameLine(); ImGui::TextDisabled("(seat 0 = kbd+mouse, 1-3 = controllers)");
        ImGui::InputText("IWAD", iwad, sizeof(iwad));
        ImGui::InputText("Game path", gamePath, sizeof(gamePath));
        ImGui::SameLine(); ImGui::TextDisabled("(blank = sibling build)");

        ImGui::Spacing();
        ImGui::SliderFloat("Render scale", &scale, 0.25f, 2.0f, "%.2f");
        ImGui::SameLine(); ImGui::TextDisabled("(<1 faster/softer, >1 supersample)");
        ImGui::Combo("FPS cap", &fpsMode, "Monitor refresh\0Uncapped\0Custom\0");
        if (fpsMode == 2) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120);
            ImGui::InputInt("##fpsCustom", &fpsCustom);
            if (fpsCustom < 1) fpsCustom = 1;
        }

        ImGui::Spacing();
        ImGui::PopItemWidth();

        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

        if (ImGui::Button("Play", ImVec2(120, 40)))
            Launch(players, iwad, gamePath, scale, fpsMode, fpsCustom);
        ImGui::Spacing();
        ImGui::TextDisabled("First launch builds the DLL + host (once). Close the game window to stop everything.");

        if (g_status[0]) {
            ImGui::Spacing();
            ImGui::TextWrapped("%s", g_status);
        }
        ImGui::End();

        ImGui::Render();
        int w, h; glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.09f, 0.09f, 0.11f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
