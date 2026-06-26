# splitdronum launcher -- Dear ImGui (GLFW + OpenGL3), cross-platform.
# Builds the vendored GLFW + ImGui and links a single self-contained launcher executable.
# Output goes to <repo>/build/launcher.exe (alongside host.exe). Driven by build-launcher.ps1 on
# Windows; on Linux/macOS just: cmake -S launcher -B build/launcher-cmake && cmake --build ...
cmake_minimum_required(VERSION 3.16)
project(splitdronum_launcher CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(TP ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)

# --- GLFW: build only the library (no examples/tests/docs) ---
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
add_subdirectory(${TP}/glfw ${CMAKE_BINARY_DIR}/glfw)

# --- Dear ImGui: core + GLFW/OpenGL3 backends (built-in GL loader, no GLEW/glad) ---
add_library(imgui STATIC
    ${TP}/imgui/imgui.cpp
    ${TP}/imgui/imgui_draw.cpp
    ${TP}/imgui/imgui_tables.cpp
    ${TP}/imgui/imgui_widgets.cpp
    ${TP}/imgui/backends/imgui_impl_glfw.cpp
    ${TP}/imgui/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${TP}/imgui ${TP}/imgui/backends ${TP}/glfw/include)

find_package(OpenGL REQUIRED)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

# --- the launcher executable ---
add_executable(launcher launcher.cpp)
target_link_libraries(launcher PRIVATE imgui)
if(WIN32)
    set_target_properties(launcher PROPERTIES WIN32_EXECUTABLE TRUE)   # no console window
    if(MSVC)
        target_link_options(launcher PRIVATE /ENTRY:mainCRTStartup)    # ...but keep plain main()
    endif()
endif()

# drop launcher.exe straight into <repo>/build (next to host.exe), for every config
set(OUT ${CMAKE_CURRENT_SOURCE_DIR}/../build)
set_target_properties(launcher PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY         ${OUT}
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${OUT}
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   ${OUT})
