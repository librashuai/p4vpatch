cmake_minimum_required(VERSION 3.21)
project(p4vpatch LANGUAGES CXX RC)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "P4V patch requires 64-bit Windows")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(p4vpatch-launcher src/launcher.cpp src/launcher.rc)
target_compile_definitions(p4vpatch-launcher PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(p4vpatch-launcher PRIVATE user32)
if(MINGW)
    target_link_options(p4vpatch-launcher PRIVATE -municode)
endif()

# P4V bundles Qt 6.8.6; the public MSVC SDK is Qt 6.8.3 (same 6.8 ABI).
# Do not link against a different compiler's Qt or a different minor series.
option(BUILD_PATCH "Build the injected Qt module" ON)
if(BUILD_PATCH)
    if(NOT MSVC)
        message(FATAL_ERROR "Patch DLL requires MSVC; set -DBUILD_PATCH=OFF to build the launcher only")
    endif()
    find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Core Widgets WebChannel WebEngineWidgets)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    add_library(p4vpatch MODULE src/patch.cpp src/terminal.cpp src/qt_focus_hook.cpp src/terminal_resources.qrc)
    target_compile_definitions(p4vpatch PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(p4vpatch PRIVATE Qt6::Core Qt6::Widgets Qt6::WebChannel Qt6::WebEngineWidgets)
    set_target_properties(p4vpatch PROPERTIES PREFIX "" OUTPUT_NAME "p4vpatch")
endif()
