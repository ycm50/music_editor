# 自检工具 (纯 C++, 无 Qt 依赖)
#
#   test_syntax   记法单元测试: 每种写法一条用例 (含旧格式兼容、容错/严格两档)
#   check_prompt  断言 core/ai_prompt 里的提示词示例合法 (可解析/小节对齐/力度有起伏/无削波)
#
# 默认构建 (-DMUSIC_BUILD_TOOLS=ON), 可用 -DMUSIC_BUILD_TOOLS=OFF 关闭。
# CI 上这两个是"改了记法或提示词就必须过"的闸门。

add_executable(test_syntax test_syntax.cpp)
target_link_libraries(test_syntax PRIVATE music-core)

add_executable(check_prompt check_prompt.cpp)
target_link_libraries(check_prompt PRIVATE music-core)

# ── 注册到 CTest: CI 上一条 `ctest --output-on-failure` 即可全跑 ──
# (enable_testing() 由根 CMakeLists 调用)
add_test(NAME syntax COMMAND test_syntax)
add_test(NAME prompt COMMAND check_prompt)

# 校验仓库内所有乐谱: 严格解析 + 小节对齐 + 无削波/直流
# MUSIC_CORE_DIR / MUSIC_REPO_ROOT 由上层 CMake 传入 (工作区可能在符号链接/联接路径下,
# 所以不能靠 ${CMAKE_CURRENT_SOURCE_DIR}/.. 反推)
if(NOT DEFINED MUSIC_REPO_ROOT)
    message(FATAL_ERROR "tools/CMakeLists.txt 需要 MUSIC_REPO_ROOT (由 music_editor/CMakeLists.txt 提供)")
endif()
set(_scores "${MUSIC_REPO_ROOT}/music_editor")

add_test(NAME score_files COMMAND save --check --quiet
         "${_scores}/1.rcp"
         "${_scores}/twinkle.rcp")

# demos 与安卓内置示例 (存在才注册, 便于裁剪过的检出)
foreach(_demo IN ITEMS
        01_piano_nocturne 02_timbre_compare 03_musicbox_jingle_bells
        04_organ_minuet 05_harp_pentatonic 06_piano_chords)
    if(EXISTS "${MUSIC_REPO_ROOT}/demos/${_demo}.rcp")
        add_test(NAME score_${_demo}
                 COMMAND save --check --quiet "${MUSIC_REPO_ROOT}/demos/${_demo}.rcp")
    endif()
endforeach()

if(EXISTS "${MUSIC_REPO_ROOT}/android/app/src/main/assets/sample.rcp")
    add_test(NAME score_android_sample
             COMMAND save --check --quiet
                     "${MUSIC_REPO_ROOT}/android/app/src/main/assets/sample.rcp")
endif()
