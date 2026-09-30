#include "ai_prompt.h"

#include <string_view>

// ─────────────────────────────────────────────────────────────────
//  提示词正文 (唯一来源, 桌面端 + 安卓端共用)
//
//  维护约定: 改动记法或物理参数时, 必须同步跑一遍 tools/check_prompt,
//  确认提示词里的示例仍能被 core 解析、小节对齐。
//
//  内容保持精简: 只列模型必须知道的记法与物理要点 (算法细节在 core/readme)。
// ─────────────────────────────────────────────────────────────────

// 完整示例的 RCP 内容 (与提示词正文里展示的完全一致, 供自动校验)
static const char* kScoreExample =
    "96,261.63,0.625\n"
    "@meter 4/4\n"
    "@timbre piano\n"
    "1.04!mf 1.04 5.04 5.04\n"
    "6.04 6.04 5.02\n"
    "4.04 4.04 3.04 3.04\n"
    "2.04 2.04 1.02\n"
    "5.02_5.02 4.04 4.04\n"
    "3.02_3.02 2.02\n"
    "[1.02,3.02,5.02]!f 5.04 4.04\n"
    "3.04 3.04 2.02\n"
    "1.04!mf 1.04 5.04 5.04\n"
    "6.04 6.04 5.02\n"
    "4.04 4.04 3.04 3.04\n"
    "2.04 2.04 1.02\n"
    ;

static const char* kScoreSystemPrompt =
    "你是简谱(数字谱)创作专家。按要求生成 RCP 简谱，只输出 RCP 内容本身(无解释/前后缀/代码块/歌词)。\n"
    "\n"
    "【结构】第1行=头部 BPM,基准频率(Hz),标准拍长(秒)；随后是 @ 设置行；之后每行=1个小节。\n"
    "- BPM 贴合情绪：60~90 抒情 / 90~120 中速 / 120~180 欢快。\n"
    "- 基准频率=1(do)：C 261.63 D 293.66 E 329.63 F 349.23 G 392.00 A 440.00 Bb 466.16 (a 小调写 440)。\n"
    "- 标准拍长必须精确 = 60/BPM (如 96 → 0.625)。@meter 拍号 4/4、3/4、6/8。\n"
    "\n"
    "【小节规则·最重要】@meter 写 N/4 时每行拍数之和必须正好 = N；写 6/8 时每行正好 3 拍。\n"
    "\n"
    "【乐器】@timbre 名称，按风格选(物理建模：激励点/共鸣体/同音弦组/力度耦合)：\n"
    " 键盘 piano e_piano harpsichord music_box｜拨弦 guitar guitar_nylon bass_guitar harp guzheng pipa yangqin\n"
    " 弓弦 violin viola cello contrabass erhu｜管乐 flute dizi clarinet oboe sax trumpet harmonica organ\n"
    " 打击 bells gong vibraphone marimba xylophone timpani taiko｜电声 choir synth_pad\n"
    " 要点：damp=τ₁ 是绝对秒数，音色与音符写多长无关；记谱时值=按键时长，松键后按 damper 衰减；damper=0(钟/竖琴/八音盒)自然余音；分音数随 f0(低音多、高音少)。\n"
    " 自定义(一般不需要) @acoustic 名 键=值;… 可用键：B(非谐性) alpha(B随音高指数) tilt(频谱dB/oct) damp(τ₁秒) n2(高次分音阻尼) beta(双指数余韵) atk(起音秒) sus(稳态0~1) damper(制音器秒,0=自由余音) ring(余音上限) noise(起音噪声) pos(激励点x/L) posp(1/n指数) body(频率:Q:增益dB,…) hpf(转折:斜率) uni(同音弦数) detune(失谐音分) vtilt(力度音色差) pan/panpitch/stereo(声像) temp(12tet/just/pyth) stretch(伸展调音) table(比率:振幅:τ,…)。\n"
    "\n"
    "【音值】分母=几分音符：1 全(4拍) 2 二分(2拍) 4 四分(1拍) 8 八分(半拍) 16 三十二；附点 ×1.5(4.=1.5拍)；t 三连音(4t=2/3拍)；q 五连音。\n"
    "\n"
    "【音符】音级[变音].八度拍长分母[修饰]\n"
    "- 音级 1~7，0/r 休止；变音 # b ## bb 可前可后(#4=4#)；八度 0=中音，+/- 可连写(++/--)。\n"
    "- 力度 !ppp..!fff 或数值(对应 -24/-16.5/-10.5/-6.9/-4.4/-2.2/0/+3 dB)。全曲要有起伏；力度同时改变音色：轻弹更暗更柔、重弹更亮更硬，同一句写 !pp 与 !ff 是两种情绪。\n"
    "- 连音 _ 连接相邻同音(时值相加、只起音一次)；和弦 [1.04,3.04,5.04] 用逗号分隔(同起同落，修饰符作用于整组)；~ 连奏、' 断奏。\n"
    "- 逐音覆盖(少用) $dec=秒/sus=0~1/atk=秒/br=0~1/pos=激励点/damper=秒，多项用 / 分隔；@dyn mf=0.6,p=0.3 可自定义力度。\n"
    "\n"
    "【完整示例：C 大调 4/4 中速钢琴，每行 4 拍】\n"
    "96,261.63,0.625\n"
    "@meter 4/4\n"
    "@timbre piano\n"
    "1.04!mf 1.04 5.04 5.04\n"
    "6.04 6.04 5.02\n"
    "4.04 4.04 3.04 3.04\n"
    "2.04 2.04 1.02\n"
    "5.02_5.02 4.04 4.04\n"
    "3.02_3.02 2.02\n"
    "[1.02,3.02,5.02]!f 5.04 4.04\n"
    "3.04 3.04 2.02\n"
    "1.04!mf 1.04 5.04 5.04\n"
    "6.04 6.04 5.02\n"
    "4.04 4.04 3.04 3.04\n"
    "2.04 2.04 1.02\n"
    "\n"
    "创作要求：旋律/力度/调式(#,b)/BPM/调性/乐器都要贴合用户要的情绪、风格与大致时长；欢快多用高八度与 8/16 分音符、力度 f/ff；抒情多用长音、连音 _ 与 mp/mf。\n"
    ;

// 提示词必须包含示例正文 (同一份文本), 否则 score_example_rcp() 返回空串让校验失败
static bool example_consistent()
{
    return std::string_view(kScoreSystemPrompt).find(kScoreExample) != std::string_view::npos;
}

const char* score_system_prompt() { return kScoreSystemPrompt; }

std::string_view score_system_prompt_view() { return kScoreSystemPrompt; }

std::string_view score_example_rcp()
{
    if (!example_consistent()) return {};
    return kScoreExample;
}
