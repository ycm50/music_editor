#ifndef MUSIC_NOTE_BUILDER_H
#define MUSIC_NOTE_BUILDER_H

#include "note_parser.h"

#include <string>
#include <string_view>
#include <vector>

// ─────────────────────────────────────────────────────────────────
//  插入面板的共享构造器 (桌面端 / 安卓端 / 测试共用同一套逻辑)
//
//  UI 只负责勾选与填参数, token 文本一律由这里生成, 保证两端一致,
//  并且生成结果一定能被解析器接受 (tools/test_builder 会逐一断言)。
// ─────────────────────────────────────────────────────────────────

/// 一个音符的完整规格 (勾选项 → RCP token)
struct NoteSpec {
    int    degree      = 1;     ///< 1~7; 0 = 休止符
    int    accidental  = 0;     ///< -2..+2 (# = +1, b = -1)
    int    octave      = 0;     ///< -4..+4 (0 = 中音)
    int    denominator = 4;     ///< 1,2,4,8,16,32
    bool   dotted      = false; ///< 附点 ×1.5
    int    tuplet      = 0;     ///< 0 = 无, 3 = 三连音(t), 5 = 五连音(q)
    bool   use_velocity = true; ///< 是否输出 !力度
    int    velocity_id  = 6;    ///< 力度名下标 (velocity_names()); 6 = "ff"
    double velocity     = 1.0;  ///< 数值力度 (0~4); velocity_id < 0 时使用
    int    articulation = 0;    ///< 0 = 自动, 1 = 连奏(~), 2 = 断奏(')
    bool   tie          = false;///< 前置 '_' (与前一个同音相连)

    // 逐音物理覆盖 (勾选才输出)
    bool   ov_atk = false; double atk = 0.010;
    bool   ov_dec = false; double dec = 1.000;
    bool   ov_sus = false; double sus = 0.500;
    bool   ov_br  = false; double br  = 0.700;
    bool   ov_B   = false; double B   = 1e-4;
    bool   ov_pos = false; double pos = 0.125;
    bool   ov_damper = false; double damper = 0.080;

    /// 勾选"和弦"时纵向叠加的其它音 (自身的力度/修饰不重复输出)
    std::vector<NoteSpec> chord;
    /// 和弦成员的"主干"文本 (如 "3.04"), 与 chord 二选一 (UI 用这个)
    std::vector<std::string> chord_tokens;

    /// 勾选"重复 xN"时把 token 用空格连着写 N 次 (方便快速铺旋律)
    int    repeat = 1;

    /// 勾选"多声部"时在 token 前插入 @voice N 行
    int    voice = 0;           ///< 0 = 不插入
};

/// 生成一个音符 token, 例如 "[1.04,3.04]!mf$pos=0.125"
std::string render_note_token(const NoteSpec& spec);

/// 只生成"主干"(音级/变音/八度/拍长, 不含 !力度 等修饰), UI 用它做和弦成员
std::string render_note_core(const NoteSpec& spec);

/// 生成 @timbre 行
std::string render_timbre_directive(std::string_view instrument);

/// 生成 @acoustic 行: 只写出与默认值不同 / 被显式设置的键
std::string render_acoustic_directive(const AcousticParams& p);

/// 力度名 (与解析器同一份表): ppp pp p mp mf f ff fff
const std::vector<std::string>& velocity_names();

/// 内置乐器目录 (UI 下拉/预设面板用)
struct InstrumentInfo {
    std::string name;      ///< @timbre 名称
    std::string category;  ///< 键盘/拨弦/弓弦/管乐/打击/电声
    std::string desc;      ///< 一句话说明
};
std::vector<InstrumentInfo> instrument_catalog();

#endif // MUSIC_NOTE_BUILDER_H
