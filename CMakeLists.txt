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
    core/ss_profile.cpp)
target_include_directories(ss_core PUBLIC core)

# --- tests: doctest (header-only, vendored), cross-platform; run via ctest ---
enable_testing()
add_executable(ss_tests
    tests/test_layout.cpp
    tests/test_join.cpp
    tests/test_profile.cpp)
target_link_libraries(ss_tests PRIVATE ss_core)
target_include_directories(ss_tests PRIVATE third_party)
add_test(NAME ss_tests COMMAND ss_tests)
