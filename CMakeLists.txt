# splitdronum -- cross-platform build (Windows + macOS + Linux). The per-component .ps1 scripts still
# work on Windows for local dev; this is the portable build the CI matrix and the macOS port use.
#
# Stage 1 (here): core logic + tests -- pure C++17, no platform deps, builds everywhere.
# Later stages add the SDL2 host/compositor and the Qt launcher.
cmake_minimum_required(VERSION 3.16)
project(splitdronum LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# --- core: pure portable logic (layout / join / profile), zero platform/engine headers ---
add_library(ss_core STATIC
    core/ss_layout.cpp
    core/ss_join.cpp
    core/ss_profile.cpp
    core/ss_input.cpp)
target_include_directories(ss_core PUBLIC core)

# --- tests: doctest (header-only, vendored), cross-platform; run via ctest ---
enable_testing()
add_executable(ss_tests
    tests/test_layout.cpp
    tests/test_join.cpp
    tests/test_profile.cpp
    tests/test_input.cpp)
target_link_libraries(ss_tests PRIVATE ss_core)
target_include_directories(ss_tests PRIVATE third_party)
add_test(NAME ss_tests COMMAND ss_tests)

# --- launcher: Qt Quick GUI, cross-platform. Off by default so the core/tests build needs no Qt;
#     CI (and local Qt builds) turn it on with -DSS_BUILD_LAUNCHER=ON after installing Qt. ---
option(SS_BUILD_LAUNCHER "Build the Qt Quick launcher (requires Qt6)" OFF)
if(SS_BUILD_LAUNCHER)
    add_subdirectory(launcher)
endif()

# --- host: the cross-platform Qt host/compositor (Windows + macOS). Off by default so the core/tests
#     build stays dependency-free; CI turns it on with -DSS_BUILD_HOST=ON. Qt Quick is the compositor:
#     it renders the seat framebuffers AND the QML overlay in one scene, on Qt's RHI (D3D11 on Windows,
#     Metal on macOS) -- so there is no platform-specific GPU code. SDL2 is fetched + built STATIC and
#     used ONLY for gamepad input (Qt 6 has no gamepad module); no SDL window or renderer. This is the
#     multiplatform host the release pipeline packages (the old Win32/D3D11 host has been retired). ---
option(SS_BUILD_HOST "Build the cross-platform Qt host/compositor (requires Qt6; fetches SDL2 for pads)" OFF)
# --- the macOS engine capture dylib (dll/ss_hook_mac.cpp). Compile-check target: it injects into the
#     stock engine at runtime (DYLD_INSERT_LIBRARIES) and resolves SDL/GL from the engine, so here we
#     only verify it compiles + links (with -undefined dynamic_lookup). The real deploy build is
#     build-dll.sh (which also extracts engine offsets via gen_offsets_mac). ---
option(SS_BUILD_HOOK "Build the macOS capture dylib (compile check; macOS only)" OFF)

# SDL2 (static): the host links it for gamepads; the mac hook only uses its HEADERS (SDL resolves from
# the engine at runtime). Fetch once if either target needs it.
if(SS_BUILD_HOST OR (SS_BUILD_HOOK AND APPLE))
    include(FetchContent)
    set(SDL_SHARED      OFF CACHE BOOL "" FORCE)   # static: fold SDL into the host binary
    set(SDL_STATIC      ON  CACHE BOOL "" FORCE)
    set(SDL_TEST        OFF CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL2
        URL https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz)
    FetchContent_MakeAvailable(SDL2)
endif()

if(SS_BUILD_HOST)
    find_package(Qt6 REQUIRED COMPONENTS Quick Gui Qml)

    set(SS_HOST_SOURCES host/host_qt.cpp)
    if(WIN32)
        list(APPEND SS_HOST_SOURCES host/host.rc)   # app/window icon (the splitdronum skull)
    endif()
    add_executable(sshost ${SS_HOST_SOURCES})
    set_target_properties(sshost PROPERTIES AUTOMOC ON OUTPUT_NAME host)   # ships as host(.exe)
    target_link_libraries(sshost PRIVATE ss_core SDL2::SDL2-static Qt6::Quick Qt6::Gui Qt6::Qml)
    target_include_directories(sshost PRIVATE core)
endif()

if(SS_BUILD_HOOK AND APPLE)
    # ss_offsets.h is generated from the engine (build-dll.sh). For a compile check without the engine,
    # fall back to a zeroed stub so the source builds; the real header replaces it at deploy time.
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/dll/ss_offsets.h)
        set(SS_OFFSETS_INC ${CMAKE_CURRENT_SOURCE_DIR}/dll)
    else()
        set(SS_OFFSETS_INC ${CMAKE_BINARY_DIR}/ss_offsets_stub)
        file(WRITE ${SS_OFFSETS_INC}/ss_offsets.h
            "#pragma once\n"
            "#define SS_RVA_D_PostEvent 0ULL\n#define SS_RVA_menuactive 0ULL\n"
            "#define SS_RVA_ConsoleState 0ULL\n#define SS_RVA_GUICapture 0ULL\n"
            "#define SS_RVA_g_ulChatMode 0ULL\n#define SS_RVA_AppActive 0ULL\n"
            "#define SS_RVA_AddCommandString 0ULL\n")
    endif()
    add_library(sshook MODULE dll/ss_hook_mac.cpp)
    set_target_properties(sshook PROPERTIES OUTPUT_NAME ss_hook SUFFIX ".dylib" OSX_ARCHITECTURES x86_64)
    target_include_directories(sshook PRIVATE ${sdl2_SOURCE_DIR}/include ${SS_OFFSETS_INC} dll host)
    target_link_libraries(sshook PRIVATE "-framework OpenGL")
    target_link_options(sshook PRIVATE -undefined dynamic_lookup)
endif()
