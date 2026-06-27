cmake_minimum_required(VERSION 3.16)
project(overlay_preview LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Quick Gui)

add_executable(overlay_preview main.cpp)
target_link_libraries(overlay_preview PRIVATE Qt6::Quick Qt6::Gui)
