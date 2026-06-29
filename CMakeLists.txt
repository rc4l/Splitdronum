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
if(SS_BUILD_HOST)
    include(FetchContent)
    set(SDL_SHARED      OFF CACHE BOOL "" FORCE)   # static: fold SDL into the host binary
    set(SDL_STATIC      ON  CACHE BOOL "" FORCE)
    set(SDL_TEST        OFF CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL2
        URL https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz)
    FetchContent_MakeAvailable(SDL2)

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
