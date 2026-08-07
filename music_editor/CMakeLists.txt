cmake_minimum_required(VERSION 3.16)
project(music-editor VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# ── Core static library (no Qt dependency) ──────────────────────────
add_subdirectory(core)

# ── Player executable (Qt Multimedia) ──────────────────────────────
find_package(Qt6 COMPONENTS Multimedia QUIET)
if(NOT Qt6_FOUND)
    # 区分: Qt6 整体缺失 vs 已装 Qt6 但缺 Multimedia 组件
    find_package(Qt6 QUIET)
    if(Qt6_FOUND)
        message(FATAL_ERROR "Qt6 已找到但缺少 Multimedia 组件, 请安装 qtmultimedia")
    endif()
    find_package(Qt5 5.15 REQUIRED COMPONENTS Multimedia)
    set(QT_VERSION_MAJOR 5)
else()
    set(QT_VERSION_MAJOR 6)
endif()

add_subdirectory(player)

# ── Save executable (no Qt, standalone) ────────────────────────────
add_subdirectory(save)

# ── UI executable (Qt Widgets) ─────────────────────────────────────
find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Widgets Network REQUIRED)
add_subdirectory(ui)
