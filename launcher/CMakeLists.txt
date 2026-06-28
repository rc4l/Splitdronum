# splitdronum launcher -- a Qt Quick GUI over play.ps1 (same Qt 6 stack as the in-game overlay).
# Output goes to <repo>/build/launcher.exe (alongside host.exe). Driven by build-launcher.ps1, which
# points CMAKE_PREFIX_PATH at deps/qt/... and runs windeployqt so the Qt DLLs sit next to the exe.
cmake_minimum_required(VERSION 3.16)
project(splitdronum_launcher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Quick Gui QuickControls2)

# launcher.rc gives the EXE its Windows file icon; the .qrc embeds the QML + logo + icon.
set(LAUNCHER_SOURCES launcher.cpp launcher.qrc)
if(WIN32)
    list(APPEND LAUNCHER_SOURCES launcher.rc)
endif()

add_executable(launcher ${LAUNCHER_SOURCES})
target_link_libraries(launcher PRIVATE Qt6::Quick Qt6::Gui Qt6::QuickControls2)

if(WIN32)
    set_target_properties(launcher PROPERTIES WIN32_EXECUTABLE TRUE)   # no console window
endif()

# When built STANDALONE (build-launcher.ps1), drop launcher.exe straight into <repo>/build next to host.exe.
# When built as part of the root CMake project, leave the default output dir -- forcing ../build there would
# collide with the build/launcher subdir CMake creates for this target (the executable path resolves to '.').
if(PROJECT_IS_TOP_LEVEL)
    set(OUT ${CMAKE_CURRENT_SOURCE_DIR}/../build)
    set_target_properties(launcher PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY         ${OUT}
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${OUT}
        RUNTIME_OUTPUT_DIRECTORY_DEBUG   ${OUT})
endif()
