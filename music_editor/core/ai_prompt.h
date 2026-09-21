#ifndef MUSIC_AI_PROMPT_H
#define MUSIC_AI_PROMPT_H

#include <string_view>

/**
 * AI 生成乐谱用的系统提示词 (单一来源)
 *
 * 桌面端 (ui) 与安卓端 (JNI) 都从这里取, 避免两端各写一份而漂移。
 * 文案本身必须与 core 的解析器保持一致 —— 提示词里出现的每一种写法,
 * 都必须能被 parse_rcp 正确解析 (可用 tools/check_prompt 或 --analyze 验证)。
 */
const char* score_system_prompt();

/// 同上, 返回 string_view (便于 C++ 侧直接使用)
std::string_view score_system_prompt_view();

/**
 * 提示词里那段"完整示例"的 RCP 内容 (不含说明文字)
 *
 * 单独抽出来是为了能被自动化校验: tools/check_prompt 会解析它、渲染它,
 * 断言小节对齐 / 无警告 / 力度有起伏。改提示词时请一起跑这个校验。
 */
std::string_view score_example_rcp();

#endif // MUSIC_AI_PROMPT_H
