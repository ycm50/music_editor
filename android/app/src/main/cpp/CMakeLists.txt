cmake_minimum_required(VERSION 3.16)
project(music-editor-android VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── 纯 C++ 核心库 (与桌面版 music_editor/core 同源, 复用同一份源码) ──
# note_parser / tone_gen / wav_writer 不依赖任何 Android/Qt 特性
# monorepo 内直接引用桌面端源码目录, 避免两份拷贝漂移
# 从 android/app/src/main/cpp 上溯 5 层到仓库根: ../../../../../music_editor/core
set(MUSIC_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../music_editor/core")
if(NOT EXISTS "${MUSIC_CORE_DIR}/note_parser.cpp")
    message(FATAL_ERROR "找不到共享 core 目录: ${MUSIC_CORE_DIR}")
endif()
add_library(music-core STATIC
    ${MUSIC_CORE_DIR}/note_parser.cpp
    ${MUSIC_CORE_DIR}/tone_gen.cpp
    ${MUSIC_CORE_DIR}/wav_writer.cpp
)
target_include_directories(music-core PUBLIC ${MUSIC_CORE_DIR})

# ── JNI 桥接层: 把核心能力暴露给 Kotlin/Java ─────────────────────
add_library(music-native SHARED
    jni_bridge.cpp
)
target_link_libraries(music-native PRIVATE music-core)

# Android 日志 (__android_log_print)
find_library(log-lib log)
target_link_libraries(music-native PRIVATE ${log-lib})
