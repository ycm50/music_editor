#include "note_parser.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

namespace {

// ── 通用小工具 ────────────────────────────────────────────────────
std::string trim_ws(std::string_view s)
{
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos) return {};
    auto e = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(b, e - b + 1));
}

std::string lower(std::string_view s)
{
    std::string r;
    r.reserve(s.size());
    for (char c : s)
        r.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return r;
}

[[noreturn]] void fail(const std::string& msg)
{
    throw std::invalid_argument(msg);
}

/// 严格解析一个 double (整串必须消费完)
double parse_double(std::string_view sv, const std::string& what)
{
    std::string s = trim_ws(sv);
    if (s.empty()) fail("missing number for " + what);
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0')
        fail("invalid number for " + what + ": " + s);
    return v;
}

// ── 力度表 (dB 语义) ─────────────────────────────────────────────
// 振幅即相对 !ff 的线性倍数, 注释里标注对应的 dB —— 与录音/DAW 对齐,
// 并允许 @dyn 覆盖、支持 ppp/fff 扩展。
struct DynEntry { const char* name; double amp; };
const DynEntry kDyn[] = {
    {"ppp", 0.063096},   // -24.0 dB
    {"pp",  0.15},       // -16.5 dB
    {"p",   0.30},       // -10.5 dB
    {"mp",  0.45},       //  -6.9 dB
    {"mf",  0.60},       //  -4.4 dB
    {"f",   0.78},       //  -2.2 dB
    {"ff",  1.00},       //   0.0 dB
    {"fff", 1.412538},   //  +3.0 dB
};

/// 去掉 token 上 "连音/物理/力度" 之后的所有修饰, 返回主干 "音.八度分母[修饰]"
/// 同时把修饰解析进 Note。dyn 为 @dyn 自定义力度表 (可为空)。
void parse_modifiers(std::string_view tail, Note& n,
                     const std::unordered_map<std::string, double>* dyn)
{
    size_t i = 0;
    while (i < tail.size()) {
        char c = tail[i];
        if (c == '!') {
            // 力度
            size_t j = i + 1;
            while (j < tail.size() && tail[j] != '!' && tail[j] != '@'
                   && tail[j] != '$' && tail[j] != '~' && tail[j] != '\'')
                ++j;
            std::string name = lower(tail.substr(i + 1, j - i - 1));
            if (name.empty()) fail("empty dynamic after '!'");
            char* end = nullptr;
            double v = std::strtod(name.c_str(), &end);
            if (end != name.c_str() && *end == '\0') {
                n.velocity = v;                 // 直接写数值: 线性振幅
            } else if (dyn && dyn->count(name)) {
                n.velocity = dyn->at(name);     // @dyn 自定义
            } else {
                double db = 0.0;
                if (!dynamic_db(name, db))
                    fail("unknown dynamic: !" + name
                         + " (可用 ppp..fff / 0~4 数值 / @dyn 自定义)");
                n.velocity = db_to_amplitude(db);
            }
            if (n.velocity < 0.0 || n.velocity > 4.0)
                fail("dynamic out of range [0,4]: " + name);
            i = j;
        } else if (c == '$') {
            // 逐音物理覆盖: $dec=0.3[/sus=0.5] (用 '/' 分隔多个, ',' 留给逐分音列表)
            size_t j = i + 1;
            while (j < tail.size() && tail[j] != '!' && tail[j] != '@' && tail[j] != '$')
                ++j;
            std::string spec = std::string(tail.substr(i + 1, j - i - 1));
            size_t p = 0;
            while (p <= spec.size()) {
                size_t q = spec.find('/', p);
                std::string item = spec.substr(p, (q == std::string::npos ? spec.size() : q) - p);
                if (!item.empty()) {
                    auto eq = item.find('=');
                    if (eq == std::string::npos) fail("bad $item (need k=v): " + item);
                    std::string k = lower(trim_ws(std::string_view(item).substr(0, eq)));
                    double v = parse_double(std::string_view(item).substr(eq + 1), "$" + k);
                    if      (k == "atk") { n.ov.has_atk = true; n.ov.atk = v; }
                    else if (k == "dec" || k == "decay") { n.ov.has_dec = true; n.ov.dec = v; }
                    else if (k == "sus") { n.ov.has_sus = true; n.ov.sus = v; }
                    else if (k == "rel") { n.ov.has_rel = true; n.ov.rel = v; }
                    else if (k == "br")  { n.ov.has_br  = true; n.ov.br  = v; }
                    else if (k == "b")   { n.ov.has_B   = true; n.ov.B   = v; }
                    else if (k == "pos")   { n.ov.has_pos   = true; n.ov.pos   = v; }
                    else if (k == "tilt")  { n.ov.has_tilt  = true; n.ov.tilt  = v; }
                    else if (k == "damper" || k == "dmp") { n.ov.has_damper = true; n.ov.damper = v; }
                    else if (k == "ring")  { n.ov.has_ring  = true; n.ov.ring  = v; }
                    else if (k == "n2")    { n.ov.has_n2    = true; n.ov.n2    = v; }
                    else if (k == "uni" || k == "unison") { n.ov.has_uni = true; n.ov.uni = static_cast<int>(v); }
                    else if (k == "detune"){ n.ov.has_detune= true; n.ov.detune= v; }
                    else if (k == "pan")   { n.ov.has_pan   = true; n.ov.pan   = v; }
                    else fail("unknown $key: " + k);
                }
                if (q == std::string::npos) break;
                p = q + 1;
            }
            i = j;
        } else if (c == '@') {
            // 逐音音色名 (仅记录, 实际音色由文档级 @timbre 决定)
            size_t j = i + 1;
            while (j < tail.size() && tail[j] != '!' && tail[j] != '$'
                   && tail[j] != '~' && tail[j] != '\'')
                ++j;
            if (j == i + 1) fail("empty timbre after '@'");
            i = j;
        } else if (c == '~') {
            n.articulation = Articulation::Legato;
            ++i;
        } else if (c == '\'') {
            n.articulation = Articulation::Staccato;
            ++i;
        } else {
            fail(std::string("unexpected modifier character '") + c + "'");
        }
    }
}

} // namespace

// ── 力度表 (公开接口) ─────────────────────────────────────────────
bool dynamic_db(std::string_view name, double& db_out)
{
    const std::string n = lower(name);
    for (const auto& d : kDyn) {
        if (n == d.name) {
            db_out = 20.0 * std::log10(d.amp);
            return true;
        }
    }
    return false;
}

double db_to_amplitude(double db)
{
    const double a = std::pow(10.0, db / 20.0);
    return a < 0.0 ? 0.0 : (a > 4.0 ? 4.0 : a);
}

const std::vector<std::string>& dynamic_names()
{
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v;
        for (const auto& d : kDyn) v.emplace_back(d.name);
        return v;
    }();
    return names;
}

// ── 音名 → 半音偏移 ──────────────────────────────────────────────
const std::unordered_map<char, int> NoteParser::NOTE_TO_SEMITONE = {
    {'1', 0}, {'2', 2}, {'3', 4}, {'4', 5},
    {'5', 7}, {'6', 9}, {'7', 11},
};

// ── 构造 ───────────────────────────────────────────────────────────
NoteParser::NoteParser(double base_freq, double base_beat_duration)
    : base_freq_(base_freq)
    , base_beat_duration_(base_beat_duration)
{
    if (!(base_freq_ > 0.0))
        throw std::invalid_argument("base frequency must be positive");
    if (!(base_beat_duration_ > 0.0))
        throw std::invalid_argument("base beat duration must be positive");
}

void NoteParser::set_bpm(double bpm)
{
    if (!(bpm > 0.0))
        throw std::invalid_argument("BPM must be positive");
    base_beat_duration_ = 60.0 / bpm;
}

// ── 频率计算 ───────────────────────────────────────────────────────
double NoteParser::frequency_of(int scale_degree, int octave_offset, int accidental) const
{
    if (scale_degree == 0) return 0.0;   // 休止符: 无频率
    auto it = NOTE_TO_SEMITONE.find(static_cast<char>('0' + scale_degree));
    if (it == NOTE_TO_SEMITONE.end())
        fail("invalid scale degree: " + std::to_string(scale_degree));
    int semitone = octave_offset * 12 + it->second + accidental;
    return base_freq_ * std::pow(2.0, semitone / 12.0);
}

// ── 解析主干的 "八度 + 拍长分母" 段 ───────────────────────────────
// 返回 {octave_offset, 有效分母}, 并把修饰部分写回 tail_out
static void parse_octave_and_beat(std::string_view s, Note& n, std::string_view& tail_out)
{
    // 八度: '0'(中音) 或 连续的 '+'/'-' (上限 ±4)
    int oct = 0;
    size_t i = 0;
    if (!s.empty() && s[0] == '0') {
        oct = 0;
        i = 1;
    } else {
        while (i < s.size() && (s[i] == '+' || s[i] == '-')) {
            oct += (s[i] == '+') ? 1 : -1;
            ++i;
        }
        if (i == 0)
            fail("missing octave marker (must start with 0/+/-)");
    }
    if (oct > 4 || oct < -4)
        fail("octave offset out of range [-4,4]");

    // 拍长分母: 只取连续数字 (分母必须是整数, 如 4/2/8/16);
    // 小数点只作"附点"修饰, 紧跟分母之后处理 —— 这样 2.08. 是附点八分音符,
    // 而 5.00:5 这类历史脏写法会在下面被明确拒绝。
    size_t ds = i;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])))
        ++i;
    if (i == ds)
        fail(std::string("missing beat denominator in \"") + std::string(s) + "\"");
    double denom = parse_double(s.substr(ds, i - ds), "beat denominator");
    if (!(denom >= 1.0))
        fail(std::string("拍长分母必须 >= 1 (4=四分, 2=二分, 8=八分, 1=全音符; 三连音写 4t): \"")
             + std::string(s) + "\"");

    // 附点 '.' 只能有一个, 且必须紧跟分母 (上面已吃掉), 倍数 ×1.5
    std::string_view tail = s.substr(i);
    if (!tail.empty() && tail[0] == '.') {
        denom /= 1.5;
        tail.remove_prefix(1);
    }
    // 连音符: 't' = 三连音 (时值 ×2/3, 三个合二拍), 'q' = 五连音 (时值 ×4/5, 五个合四拍)
    // 分母变大 → 时值变短: denom *= 1.5 等价于 duration * 2/3
    if (!tail.empty() && tail[0] == 't') { denom *= 1.5; tail.remove_prefix(1); }
    else if (!tail.empty() && tail[0] == 'q') { denom *= 1.25; tail.remove_prefix(1); }

    // 历史遗留的 ':' 连音写法 (旧实现把 ':' 换成 '.' 当小数解析, 语义已损坏, 且
    // 会把 5.00:5 读成 4 秒)。这里明确拒绝, 引导改用 '_'。
    if (!tail.empty() && tail[0] == ':')
        fail(std::string("':' 连音写法已废弃且旧语义有误, 请改用 '_' (如 1.04_1.04): \"")
             + std::string(s) + "\"");
    if (!tail.empty() && tail[0] == '-')
        fail(std::string("拍长分母为正数, 不可为负: \"") + std::string(s) + "\"");

    n.octave_offset = oct;
    n.duration_sec  = 0.0;   // 占位, 由调用方按分母换算
    tail_out = tail;

    // 分母换算成时值: 四分音符 = 标准拍长 → 拍数 = 4/分母
    // 这里把"拍数"暂存进 duration_sec, 由 parse() 乘上拍长
    n.duration_sec = 4.0 / denom;   // 单位: 拍
}

// ── 解析单个音符 ───────────────────────────────────────────────────
Note NoteParser::parse(std::string_view note_str) const
{
    Note n;
    std::string s = trim_ws(note_str);
    if (s.empty()) fail("empty note token");

    size_t dot = s.find('.');
    if (dot == std::string::npos)
        fail("invalid note format (missing '.'): " + s);

    // 音级 + 变音 (变音可写在音级前 #4 / 后 4#)
    size_t p = 0;
    std::string head = s.substr(0, dot);
    std::string tail = s.substr(dot + 1);

    if (head.empty()) fail("missing note symbol: " + s);

    // 前置变音
    while (p < head.size() && (head[p] == '#' || head[p] == 'b' || head[p] == 'n')) {
        if (head[p] == '#') n.accidental += 1;
        else if (head[p] == 'b') n.accidental -= 1;
        ++p;
    }

    bool rest = false;
    if (p < head.size() && (head[p] == '0' || head[p] == 'r' || head[p] == 'R')) {
        rest = true;
        n.scale_degree = 0;
        ++p;
    } else if (p < head.size() && NOTE_TO_SEMITONE.count(head[p])) {
        n.scale_degree = head[p] - '0';
        ++p;
    } else {
        fail(std::string("invalid note symbol in \"") + s + "\"");
    }

    // 后置变音
    if (!rest) {
        while (p < head.size()) {
            if (head[p] == '#') { n.accidental += 1; ++p; }
            else if (head[p] == 'b') { n.accidental -= 1; ++p; }
            else if (head[p] == 'n') { ++p; }               // 还原记号 (显式自然音)
            else fail(std::string("invalid accidental '") + head[p] + "' in " + s);
        }
    } else if (p != head.size()) {
        fail("rest must not carry accidental: " + s);
    }

    // 八度 + 拍长分母 + 修饰
    std::string_view tail_v{tail};
    parse_octave_and_beat(tail_v, n, tail_v);
    parse_modifiers(tail_v, n, dyn_);

    double beats = n.duration_sec;      // parse_octave_and_beat 暂存拍数
    n.rest          = rest;
    n.duration_sec  = beats * base_beat_duration_;
    n.frequency     = rest ? 0.0
                           : frequency_of(n.scale_degree, n.octave_offset, n.accidental);
    if (n.rest) { n.scale_degree = 0; n.octave_offset = 0; n.accidental = 0; }
    return n;
}

// 解析一个不含 '_' 的片段: 可能是 [和弦] 或单个音符
static std::vector<Note> parse_segment(const NoteParser& parser, std::string_view seg)
{
    std::string t = trim_ws(seg);
    if (t.empty()) throw std::invalid_argument("empty note segment");

    if (t.front() != '[') {
        if (t.find(']') != std::string::npos)
            throw std::invalid_argument("stray ']' in: " + t);
        return {parser.parse(t)};
    }

    // 和弦: [...] 之后可以跟修饰符 (!力度 / $覆盖 / @音色 / ~ / '),
    // 修饰符作用于和弦的每个成员, 因此要先按括号深度把两者分开。
    std::vector<std::string> modifiers;
    std::string head;
    {
        int depth = 0;
        size_t i = 0;
        for (; i < t.size(); ++i) {
            const char c = t[i];
            if (c == '[') { ++depth; }
            else if (c == ']') {
                --depth;
                if (depth < 0) throw std::invalid_argument("stray ']' in: " + t);
                if (depth == 0 && i + 1 < t.size()) break;
            }
        }
        if (depth != 0) throw std::invalid_argument("unbalanced '[' in chord: " + t);
        const size_t close = (i < t.size() && t[i] == ']') ? i : t.rfind(']');
        head = t.substr(0, close + 1);
        std::string rest = t.substr(close + 1);

        // rest 里若混入了空白 (如 "[a,b] !f"), 逐 token 收进来
        std::string cur;
        auto flush = [&] { if (!cur.empty()) { modifiers.push_back(cur); cur.clear(); } };
        for (char c : rest) {
            if (c == ' ' || c == '\t') { flush(); continue; }
            cur.push_back(c);
        }
        flush();
    }
    if (head.size() < 2 || head.front() != '[' || head.back() != ']')
        throw std::invalid_argument("unbalanced '[' in chord: " + t);

    std::string inner = head.substr(1, head.size() - 2);
    std::vector<Note> notes;

    // 和弦成员分隔: 推荐用 ',' ('+' 与八度记号 '+' 冲突, 仅作兼容,
    // 且仅当该 '+' 不属于八度记号时才作为分隔符)
    std::vector<std::string> members;
    {
        // 八度记号 '+' 位于"第一个 '.' 之后、分母数字之前"的那一段:
        // 这段里可以出现连续多个 '+'/'-' (如 1.++04 = 高两个八度), 都不算分隔符。
        // 分母数字一出现, 八度区即结束, 之后的 '+' 才是成员分隔符。
        std::string cur;
        bool in_octave = false;
        for (char c : inner) {
            if (c == ',') { members.push_back(cur); cur.clear(); in_octave = false; continue; }
            if (c == '+') {
                const bool is_octave_sign = in_octave || (!cur.empty() && cur.back() == '.');
                if (!is_octave_sign && !cur.empty()) {
                    members.push_back(cur);
                    cur.clear();
                    in_octave = false;
                    continue;
                }
            }
            cur.push_back(c);
            if (c == '.') in_octave = true;
            else if (c == '+' || c == '-') { /* 仍可能在八度区 */ }
            else in_octave = false;
        }
        members.push_back(cur);
    }

    for (auto& part : members) {
        std::string pt = trim_ws(part);
        if (pt.empty()) throw std::invalid_argument("empty chord member in: " + t);
        for (auto& n : parse_segment(parser, pt)) notes.push_back(std::move(n));
    }
    if (notes.empty()) throw std::invalid_argument("empty chord: " + t);

    // 把修饰符作用到每个成员
    if (!modifiers.empty()) {
        for (auto& n : notes) {
            std::string tail;
            for (const auto& m : modifiers) tail += m;
            parse_modifiers(tail, n, parser.dyn_table());
        }
    }

    // 和弦成员统一到最长时值, 便于纵向混音对齐
    double dur = 0.0;
    for (const auto& x : notes) dur = std::max(dur, x.duration_sec);
    for (auto& x : notes) x.duration_sec = dur;
    return notes;
}

// 解析完整 token: 先按 '_' 切连音段, 再逐段解析和弦/单音, 最后合并连音
std::vector<Note> NoteParser::parse_token(std::string_view token) const
{
    std::string t = trim_ws(token);
    if (t.empty()) fail("empty token");

    // '_' 绑定优先级高于 '+' (和弦括号): [a+b]_[a+b] 是两段, 不是四个成员
    std::vector<std::string> segs;
    {
        int depth = 0;
        std::string cur;
        for (size_t i = 0; i < t.size(); ++i) {
            char c = t[i];
            if (c == '[') { ++depth; cur.push_back(c); continue; }
            if (c == ']') { depth = std::max(0, depth - 1); cur.push_back(c); continue; }
            if (c == '_' && depth == 0) { segs.push_back(trim_ws(cur)); cur.clear(); continue; }
            cur.push_back(c);
        }
        segs.push_back(trim_ws(cur));
    }

    if (segs.size() == 1)
        return parse_segment(*this, segs.front());

    // 连音: 段数与每段音数必须一致, 且对应成员音高相同
    std::vector<Note> out = parse_segment(*this, segs.front());
    for (size_t si = 1; si < segs.size(); ++si) {
        std::vector<Note> nxt = parse_segment(*this, segs[si]);
        if (nxt.size() != out.size())
            fail("连音各段音数必须一致: " + t);
        for (size_t k = 0; k < out.size(); ++k) {
            if (out[k].rest != nxt[k].rest)
                fail("休止符与音符不能连音: " + t);
            if (!out[k].rest && std::abs(out[k].frequency - nxt[k].frequency) > 1e-9)
                fail("连音要求音高完全一致 (滑奏请用 ~): " + t);
            out[k].duration_sec += nxt[k].duration_sec;
        }
    }
    return out;
}

// ── 物理参数合并 (预设打底, 显式给出的键覆盖) ─────────────────────
void merge_acoustic(AcousticParams& base, const AcousticParams& o)
{
    if (o.set_mask & AF_INHARM)  base.inharmonicity = o.inharmonicity;
    if (o.set_mask & AF_DECAY) {
        base.decay = o.decay;
        base.partial_decay = o.partial_decay;
    }
    if (o.set_mask & AF_EXP)     base.damping_exp = o.damping_exp;
    if (o.set_mask & AF_ATTACK)  base.attack = o.attack;
    if (o.set_mask & AF_SUSTAIN) base.sustain = o.sustain;
    if (o.set_mask & AF_RELEASE) base.release = o.release;
    if (o.set_mask & AF_BRIGHT)  base.brightness = o.brightness;
    if (o.set_mask & AF_NOISE)   base.noise = o.noise;
    if (o.set_mask & AF_STEREO)  base.stereo = o.stereo;
    if (o.set_mask & AF_TILT)    base.tilt_db_oct = o.tilt_db_oct;
    if (o.set_mask & AF_FLOOR)   base.partial_floor = o.partial_floor;
    if (o.set_mask & AF_MAXP)    base.max_partials = o.max_partials;
    if (o.set_mask & AF_INHALPHA){ base.inharm_alpha = o.inharm_alpha;
                                   base.inharm_ref_hz = o.inharm_ref_hz; }
    if (o.set_mask & AF_STRIKE)  { base.strike_pos = o.strike_pos;
                                   base.strike_p = o.strike_p; }
    if (o.set_mask & AF_BODY)    { base.body = o.body;
                                   base.body_hp_hz = o.body_hp_hz;
                                   base.body_hp_db_oct = o.body_hp_db_oct; }
    if (o.set_mask & AF_DAMP2)   { base.damping_law = o.damping_law;
                                   base.damping_n2 = o.damping_n2; }
    if (o.set_mask & AF_DECAY2)  { base.decay2_beta = o.decay2_beta;
                                   base.decay2_ratio = o.decay2_ratio; }
    if (o.set_mask & AF_DAMPER)  base.damper = o.damper;
    if (o.set_mask & AF_RING)    base.ring = o.ring;
    if (o.set_mask & AF_NOISELEN)base.noise_len = o.noise_len;
    if (o.set_mask & AF_VELTILT) { base.vel_tilt = o.vel_tilt;
                                   base.vel_attack = o.vel_attack;
                                   base.vel_noise = o.vel_noise; }
    if (o.set_mask & AF_UNISON)  { base.unison = o.unison;
                                   base.detune_cents = o.detune_cents; }
    if (o.set_mask & AF_PHASE)   base.phase = o.phase;
    if (o.set_mask & AF_TABLE)   base.partial_table = o.partial_table;
    if (o.set_mask & AF_TEMPER)  { base.temperament = o.temperament;
                                   base.stretch = o.stretch; }
    if (o.set_mask & AF_PAN)     { base.pan = o.pan; base.pan_pitch = o.pan_pitch; }
    base.set_mask |= o.set_mask;
}

// ── 动态映射解析 (@dyn mf=0.6,p=0.3) ─────────────────────────────
namespace {
void apply_dyn_directive(std::string_view arg, RcpDocument& doc)
{
    std::stringstream ss{trim_ws(arg)};
    std::string item;
    while (std::getline(ss, item, ',')) {
        std::string it = trim_ws(item);
        if (it.empty()) continue;
        auto eq = it.find('=');
        if (eq == std::string::npos)
            fail("bad @dyn item (need name=value): " + it);
        std::string name = lower(trim_ws(std::string_view(it).substr(0, eq)));
        if (name.empty()) fail("bad @dyn item (empty name): " + it);
        double v = parse_double(std::string_view(it).substr(eq + 1), "@dyn value");
        if (v < 0.0 || v > 4.0) fail("@dyn value out of range [0,4]: " + name);
        doc.dyn[name] = v;      // 覆盖内置力度 (ppp..fff) 或新增名称
    }
}
} // namespace

// ── 物理参数解析 ──────────────────────────────────────────────────
AcousticParams parse_acoustic_params(std::string_view kv)
{
    AcousticParams p;
    std::stringstream ss{trim_ws(kv)};
    std::string item;
    while (std::getline(ss, item, ';')) {
        std::string it = trim_ws(item);
        if (it.empty()) continue;
        auto eq = it.find('=');
        if (eq == std::string::npos)
            fail("bad @acoustic item (need key=value): " + it);
        std::string k = lower(trim_ws(std::string_view(it).substr(0, eq)));
        std::string v = trim_ws(std::string_view(it).substr(eq + 1));

        auto num = [&] { return parse_double(v, "@acoustic " + k); };

        if (k == "b") {
            p.inharmonicity = num();
            p.set_mask |= AF_INHARM;
        } else if (k == "damp" || k == "decay" || k == "dec") {
            auto slash = v.find('/');
            if (slash != std::string::npos) {
                p.decay = parse_double(std::string_view(v).substr(0, slash), "@acoustic damp");
                p.damping_exp = parse_double(std::string_view(v).substr(slash + 1), "@acoustic exp");
                p.set_mask |= AF_DECAY | AF_EXP;
            } else if (v.find(',') != std::string::npos) {
                p.partial_decay.clear();
                std::stringstream vs(v);
                std::string tok;
                while (std::getline(vs, tok, ',')) {
                    std::string tt = trim_ws(tok);
                    if (tt.empty()) continue;
                    p.partial_decay.push_back(parse_double(tt, "@acoustic damp list"));
                }
                if (p.partial_decay.empty()) fail("empty @acoustic damp list");
                p.decay = p.partial_decay.front();
                p.set_mask |= AF_DECAY;
            } else {
                p.decay = num();
                p.set_mask |= AF_DECAY;
            }
        } else if (k == "exp") {
            p.damping_exp = num();
            p.set_mask |= AF_EXP;
        } else if (k == "atk") {
            p.attack = num();
            p.set_mask |= AF_ATTACK;
        } else if (k == "sus") {
            p.sustain = num();
            p.set_mask |= AF_SUSTAIN;
        } else if (k == "rel") {
            p.release = num();
            p.set_mask |= AF_RELEASE;
        } else if (k == "br") {
            p.brightness = num();
            p.set_mask |= AF_BRIGHT;
        } else if (k == "noise") {
            p.noise = num();
            p.set_mask |= AF_NOISE;
        } else if (k == "stereo") {
            p.stereo = num();
            p.set_mask |= AF_STEREO;
        } else if (k == "alpha") {
            p.inharm_alpha = num();
            p.set_mask |= AF_INHALPHA;
        } else if (k == "bref" || k == "inref") {
            p.inharm_ref_hz = num();
            p.set_mask |= AF_INHALPHA;
        } else if (k == "tilt") {
            p.tilt_db_oct = num();
            p.set_mask |= AF_TILT;
        } else if (k == "floor") {
            p.partial_floor = num();
            p.set_mask |= AF_FLOOR;
        } else if (k == "maxp" || k == "maxpart") {
            p.max_partials = static_cast<int>(num());
            p.set_mask |= AF_MAXP;
        } else if (k == "pos") {
            p.strike_pos = num();
            p.set_mask |= AF_STRIKE;
        } else if (k == "posp") {
            p.strike_p = num();
            p.set_mask |= AF_STRIKE;
        } else if (k == "body") {
            p.body.clear();
            if (lower(v) != "none" && lower(v) != "0") {
                std::stringstream vs(v);
                std::string t;
                while (std::getline(vs, t, ',')) {
                    std::string tt = trim_ws(t);
                    if (tt.empty()) continue;
                    std::vector<double> nums;
                    std::stringstream ts(tt);
                    std::string x;
                    while (std::getline(ts, x, ':'))
                        nums.push_back(parse_double(x, "@acoustic body"));
                    BodyResonance br;
                    if (nums.size() == 2) {
                        br.freq_hz = nums[0]; br.q = 6.0; br.gain_db = nums[1];
                    } else if (nums.size() == 3) {
                        br.freq_hz = nums[0]; br.q = nums[1]; br.gain_db = nums[2];
                    } else {
                        fail("@acoustic body 需要 f:q:db 或 f:db: " + tt);
                    }
                    if (!(br.freq_hz > 0.0) || !(br.q > 0.0))
                        fail("bad @acoustic body: " + tt);
                    p.body.push_back(br);
                }
            }
            p.set_mask |= AF_BODY;
        } else if (k == "hpf") {
            auto colon = v.find(':');
            if (colon == std::string::npos) {
                p.body_hp_hz = parse_double(v, "@acoustic hpf");
                p.body_hp_db_oct = 6.0;
            } else {
                p.body_hp_hz = parse_double(std::string_view(v).substr(0, colon), "@acoustic hpf");
                p.body_hp_db_oct = parse_double(std::string_view(v).substr(colon + 1), "@acoustic hpf");
            }
            p.set_mask |= AF_BODY;
        } else if (k == "n2") {
            p.damping_n2 = num();
            p.damping_law = DampingLaw::TwoTerm;
            p.set_mask |= AF_DAMP2;
        } else if (k == "law") {
            std::string lv = lower(v);
            if (lv == "exp" || lv == "pow") p.damping_law = DampingLaw::PowerExp;
            else if (lv == "n2" || lv == "two") p.damping_law = DampingLaw::TwoTerm;
            else fail("@acoustic law 只支持 exp / n2: " + v);
            p.set_mask |= AF_DAMP2;
        } else if (k == "beta") {
            p.decay2_beta = num();
            p.set_mask |= AF_DECAY2;
        } else if (k == "ratio") {
            p.decay2_ratio = num();
            p.set_mask |= AF_DECAY2;
        } else if (k == "damper" || k == "dmp") {
            p.damper = num();
            p.set_mask |= AF_DAMPER;
        } else if (k == "ring") {
            p.ring = num();
            p.set_mask |= AF_RING;
        } else if (k == "nl" || k == "noiselen") {
            p.noise_len = num();
            p.set_mask |= AF_NOISELEN;
        } else if (k == "vtilt" || k == "veltilt") {
            p.vel_tilt = num();
            p.set_mask |= AF_VELTILT;
        } else if (k == "velatk") {
            p.vel_attack = num();
            p.set_mask |= AF_VELTILT;
        } else if (k == "velnoise") {
            p.vel_noise = num();
            p.set_mask |= AF_VELTILT;
        } else if (k == "uni" || k == "unison") {
            p.unison = static_cast<int>(num());
            p.set_mask |= AF_UNISON;
        } else if (k == "detune") {
            p.detune_cents = num();
            p.set_mask |= AF_UNISON;
        } else if (k == "phase") {
            std::string pv = lower(v);
            if (pv == "coherent" || pv == "coh" || pv == "c") p.phase = PhaseMode::Coherent;
            else if (pv == "random" || pv == "rand" || pv == "r") p.phase = PhaseMode::Random;
            else fail("@acoustic phase 只支持 coherent / random: " + v);
            p.set_mask |= AF_PHASE;
        } else if (k == "table" || k == "partials") {
            p.partial_table.clear();
            if (lower(v) != "none" && lower(v) != "0") {
                std::stringstream vs(v);
                std::string t;
                while (std::getline(vs, t, ',')) {
                    std::string tt = trim_ws(t);
                    if (tt.empty()) continue;
                    std::vector<double> nums;
                    std::stringstream ts(tt);
                    std::string x;
                    while (std::getline(ts, x, ':'))
                        nums.push_back(parse_double(x, "@acoustic table"));
                    if (nums.size() != 3)
                        fail("@acoustic table 需要 ratio:amp:tau: " + tt);
                    if (!(nums[0] > 0.0) || !(nums[2] > 0.0))
                        fail("bad @acoustic table: " + tt);
                    p.partial_table.push_back({nums[0], nums[1], nums[2]});
                }
            }
            p.set_mask |= AF_TABLE;
        } else if (k == "temp" || k == "temperament") {
            std::string tv = lower(v);
            if (tv == "12tet" || tv == "12" || tv == "equal") p.temperament = Temperament::Equal12;
            else if (tv == "just" || tv == "ji") p.temperament = Temperament::Just;
            else if (tv == "pyth" || tv == "pythagorean") p.temperament = Temperament::Pythagorean;
            else fail("@acoustic temp 只支持 12tet / just / pyth: " + v);
            p.set_mask |= AF_TEMPER;
        } else if (k == "stretch") {
            p.stretch = num();
            p.set_mask |= AF_TEMPER;
        } else if (k == "pan") {
            p.pan = num();
            p.set_mask |= AF_PAN;
        } else if (k == "panpitch" || k == "pan_by_pitch") {
            p.pan_pitch = num();
            p.set_mask |= AF_PAN;
        } else {
            fail("unknown @acoustic key: " + k);
        }
    }
    if (!(p.decay > 0.0)) fail("@acoustic damp must be positive");
    if (!(p.attack >= 0.0)) fail("@acoustic atk must be >= 0");
    if (!(p.release >= 0.0)) fail("@acoustic rel must be >= 0");
    if (!(p.sustain >= 0.0 && p.sustain <= 1.0)) fail("@acoustic sus must be in [0,1]");
    if (!(p.brightness > 0.0 && p.brightness <= 1.0)) fail("@acoustic br must be in (0,1]");
    if (!(p.inharmonicity >= 0.0)) fail("@acoustic B must be >= 0");
    if (!(p.partial_floor > 0.0 && p.partial_floor <= 0.5)) fail("@acoustic floor must be in (0,0.5]");
    if (p.max_partials < 1 || p.max_partials > 1024) fail("@acoustic maxp must be in [1,1024]");
    if (!(p.strike_pos >= 0.0 && p.strike_pos <= 0.5)) fail("@acoustic pos must be in [0,0.5] (0.5 = 中央)");
    if (!(p.strike_p >= 0.0 && p.strike_p <= 4.0)) fail("@acoustic posp must be in [0,4]");
    if (!(p.body_hp_hz >= 0.0)) fail("@acoustic hpf must be >= 0");
    if (!(p.body_hp_db_oct >= 0.0)) fail("@acoustic hpf 斜率必须 >= 0 (正数表示衰减)");
    if (!(p.damping_n2 >= 0.0)) fail("@acoustic n2 must be >= 0");
    if (!(p.decay2_beta >= 0.0 && p.decay2_beta < 1.0)) fail("@acoustic beta must be in [0,1)");
    if (!(p.decay2_ratio > 0.0 && p.decay2_ratio <= 1.0)) fail("@acoustic ratio must be in (0,1]");
    if (!(p.damper >= 0.0)) fail("@acoustic damper must be >= 0 (0 = 无制音器)");
    if (!(p.ring > 0.0 && p.ring <= 60.0)) fail("@acoustic ring must be in (0,60]");
    if (!(p.noise_len > 0.0 && p.noise_len <= 1.0)) fail("@acoustic noiselen must be in (0,1]");
    if (!(p.vel_tilt >= 0.0)) fail("@acoustic vtilt must be >= 0");
    if (!(p.vel_attack >= 0.0 && p.vel_attack < 1.0)) fail("@acoustic velatk must be in [0,1)");
    if (!(p.vel_noise >= 0.0 && p.vel_noise <= 1.0)) fail("@acoustic velnoise must be in [0,1]");
    if (p.unison < 1 || p.unison > 8) fail("@acoustic uni must be in [1,8]");
    if (!(p.detune_cents >= 0.0 && p.detune_cents <= 50.0)) fail("@acoustic detune must be in [0,50] 音分");
    if (!(p.stereo >= 0.0 && p.stereo <= 1.0)) fail("@acoustic stereo must be in [0,1]");
    if (!(p.pan >= -1.0 && p.pan <= 1.0)) fail("@acoustic pan must be in [-1,1]");
    if (!(p.pan_pitch >= 0.0 && p.pan_pitch <= 1.0)) fail("@acoustic panpitch must be in [0,1]");
    if (!(p.stretch >= 0.0 && p.stretch <= 1.0)) fail("@acoustic stretch must be in [0,1]");
    return p;
}

// ── 物理预设 ─────────────────────────────────────────────────────
// 每个预设只给"有物理依据的量": 非谐性 B(含随音高的 α)、源频谱倾斜(dB/oct)、
// τ₁(基频衰减)、n2(高次分音额外阻尼, 1/τ_n = 1/τ₁ + n2·(n²−1))、制音器 τ、
// 起音/稳态、噪声、激励点 x/L、同音弦组、共鸣体共振峰与低频滚降。
namespace {
AcousticParams preset_base(const char* name, double B, double alpha, double tilt,
                           double tau1, double n2, double damper)
{
    AcousticParams p;
    p.name            = name;
    p.inharmonicity   = B;
    p.inharm_alpha    = alpha;      // B(f) = B·(f/261.63)^alpha
    p.inharm_ref_hz   = 261.625565;
    p.tilt_db_oct     = tilt;
    p.brightness      = 0.7;        // tilt 律生效时不用
    p.decay           = tau1;
    p.damping_law     = DampingLaw::TwoTerm;
    p.damping_n2      = n2;
    p.damper          = damper;
    p.attack          = 0.003;
    p.sustain         = 0.0;
    p.release         = 0.03;
    p.noise           = 0.0;
    p.noise_len       = 0.008;
    p.partial_floor   = 1e-4;
    p.max_partials    = 96;      // 够覆盖可听频段 (低音区最受益), 又不至于过慢
    p.phase           = PhaseMode::Coherent;
    p.strike_pos      = 0.0;
    p.strike_p        = 1.0;
    p.unison          = 1;
    p.detune_cents    = 0.0;
    return p;
}
} // namespace

AcousticParams Instruments::piano()
{
    // 击弦: B 随音高上升(α=1.6), 频谱 6 dB/oct, 双指数余韵, 三弦同音弦组
    AcousticParams p = preset_base("piano", 2.0e-4, 1.6, 6.0, 2.5, 0.020, 0.09);
    p.attack = 0.003; p.noise = 0.25; p.noise_len = 0.008;
    p.strike_pos = 0.125; p.strike_p = 1.6;        // 槌击点 L/8: 压制 7/8 次分音
    p.unison = 3; p.detune_cents = 0.9;            // 同音三弦微失谐 → 拍频闪烁
    p.decay2_beta = 0.30; p.decay2_ratio = 0.35;   // 先快后慢的余韵
    p.vel_tilt = 5.0; p.vel_attack = 0.45; p.vel_noise = 0.50;
    p.body_hp_hz = 85; p.body_hp_db_oct = 6.0;     // 音板低频截止
    p.body = {{180,1.4,4.0},{1400,1.2,3.0},{3200,1.0,-2.0}};
    p.pan_pitch = 0.55; p.stereo = 0.15; p.stretch = 1.0;
    return p;
}
AcousticParams Instruments::e_piano()
{
    // 电钢琴 (音叉/簧片): 更纯、更短、柔和起音
    AcousticParams p = preset_base("e_piano", 1.0e-4, 1.2, 7.0, 2.0, 0.025, 0.12);
    p.attack = 0.004; p.noise = 0.20; p.noise_len = 0.006;
    p.strike_pos = 0.15; p.strike_p = 1.4;
    p.unison = 2; p.detune_cents = 0.5;
    p.vel_tilt = 4.0; p.vel_attack = 0.35; p.vel_noise = 0.40;
    p.body_hp_hz = 70; p.body_hp_db_oct = 6.0;
    p.body = {{250,1.2,5.0},{2000,1.0,2.0}};
    p.pan_pitch = 0.50;
    return p;
}
AcousticParams Instruments::harpsichord()
{
    // 拨弦键盘: 极亮起音 + 硬拨片噪声, 无力度敏感 (巴洛克)
    AcousticParams p = preset_base("harpsichord", 1.0e-4, 1.4, 5.0, 1.4, 0.035, 0.05);
    p.attack = 0.001; p.noise = 0.30; p.noise_len = 0.006;
    p.strike_pos = 0.10; p.strike_p = 1.8;
    p.unison = 2; p.detune_cents = 1.5;
    p.vel_tilt = 2.0; p.vel_attack = 0.20; p.vel_noise = 0.30;
    p.body_hp_hz = 100; p.body_hp_db_oct = 6.0;
    p.body = {{200,1.2,5.0},{2500,1.0,4.0}};
    p.pan_pitch = 0.45;
    return p;
}
AcousticParams Instruments::music_box()
{
    // 八音盒: 金属音梳, 无制音器, 极清脆
    AcousticParams p = preset_base("music_box", 6.0e-3, 1.8, 5.0, 1.6, 0.030, 0.0);
    p.attack = 0.001; p.noise = 0.30; p.noise_len = 0.004;
    p.strike_pos = 0.12; p.strike_p = 1.4;
    p.body_hp_hz = 200; p.body_hp_db_oct = 3.0;
    p.body = {{2000,2.0,6.0},{4000,2.0,3.0}};
    p.ring = 8.0;
    return p;
}
AcousticParams Instruments::guitar()
{
    // 钢弦吉他: B 很小, 拨弦 1/5 处, 面板/背板共振
    AcousticParams p = preset_base("guitar", 1.0e-5, 1.0, 5.5, 1.1, 0.040, 0.06);
    p.attack = 0.002; p.noise = 0.35; p.noise_len = 0.008;
    p.strike_pos = 0.20; p.strike_p = 1.9;         // 理想拨弦的 1/n 包络
    p.unison = 2; p.detune_cents = 1.2;
    p.vel_tilt = 4.0; p.vel_attack = 0.30; p.vel_noise = 0.50;
    p.body_hp_hz = 100; p.body_hp_db_oct = 6.0;
    p.body = {{200,1.5,6.0},{500,1.2,3.0},{2000,1.0,2.0}};
    return p;
}
AcousticParams Instruments::guitar_nylon()
{
    // 尼龙弦吉他: 更暗更柔, 衰减略短
    AcousticParams p = preset_base("guitar_nylon", 8.0e-6, 1.0, 6.5, 0.9, 0.045, 0.08);
    p.attack = 0.003; p.noise = 0.28; p.noise_len = 0.009;
    p.strike_pos = 0.22; p.strike_p = 1.8;
    p.vel_tilt = 4.0; p.vel_attack = 0.30; p.vel_noise = 0.45;
    p.body_hp_hz = 110; p.body_hp_db_oct = 6.0;
    p.body = {{220,1.2,5.0},{900,1.0,2.0}};
    return p;
}
AcousticParams Instruments::bass_guitar()
{
    // 低音吉他: 低 B、慢衰减、低音滚降更低
    AcousticParams p = preset_base("bass_guitar", 6.0e-6, 0.8, 7.0, 1.6, 0.030, 0.07);
    p.attack = 0.003; p.noise = 0.30; p.noise_len = 0.008;
    p.strike_pos = 0.18; p.strike_p = 1.8;
    p.vel_tilt = 3.5; p.vel_attack = 0.25;
    p.body_hp_hz = 55; p.body_hp_db_oct = 6.0;
    p.body = {{120,1.2,5.0},{400,1.0,2.0}};
    return p;
}
AcousticParams Instruments::harp()
{
    // 竖琴: 无制音器, 长衰减, 晶莹
    AcousticParams p = preset_base("harp", 8.0e-5, 1.1, 5.5, 2.2, 0.011, 0.0);
    p.attack = 0.003; p.noise = 0.20; p.noise_len = 0.006;
    p.strike_pos = 0.15; p.strike_p = 1.6;
    p.body_hp_hz = 120; p.body_hp_db_oct = 6.0;
    p.body = {{300,1.2,4.0},{1500,1.0,2.0}};
    p.pan_pitch = 0.35; p.ring = 9.0;
    return p;
}
AcousticParams Instruments::guzheng()
{
    // 古筝: 铜弦/丝弦, 拨弦亮起音, 面板共振偏中频
    AcousticParams p = preset_base("guzheng", 1.0e-4, 1.2, 5.5, 2.0, 0.015, 0.05);
    p.attack = 0.002; p.noise = 0.32; p.noise_len = 0.007;
    p.strike_pos = 0.16; p.strike_p = 1.8;
    p.unison = 2; p.detune_cents = 1.5;
    p.vel_tilt = 4.5; p.vel_attack = 0.35; p.vel_noise = 0.50;
    p.body_hp_hz = 150; p.body_hp_db_oct = 6.0;
    p.body = {{400,1.2,5.0},{1600,1.0,3.0}};
    p.pan_pitch = 0.40;
    return p;
}
AcousticParams Instruments::pipa()
{
    // 琵琶: 音量大、起音更噪、衰减较快
    AcousticParams p = preset_base("pipa", 2.0e-4, 1.4, 5.0, 1.6, 0.025, 0.05);
    p.attack = 0.002; p.noise = 0.40; p.noise_len = 0.006;
    p.strike_pos = 0.18; p.strike_p = 1.8;
    p.unison = 2; p.detune_cents = 1.0;
    p.vel_tilt = 4.5; p.vel_attack = 0.35; p.vel_noise = 0.55;
    p.body_hp_hz = 130; p.body_hp_db_oct = 6.0;
    p.body = {{500,1.5,6.0},{1200,1.2,3.0}};
    return p;
}
AcousticParams Instruments::yangqin()
{
    // 扬琴: 双弦同音, 竹槌击弦, 金属感偏亮
    AcousticParams p = preset_base("yangqin", 1.0e-4, 1.3, 5.0, 1.8, 0.020, 0.04);
    p.attack = 0.002; p.noise = 0.35; p.noise_len = 0.005;
    p.strike_pos = 0.14; p.strike_p = 1.7;
    p.unison = 2; p.detune_cents = 0.8;
    p.vel_tilt = 4.0; p.vel_attack = 0.30; p.vel_noise = 0.50;
    p.body_hp_hz = 160; p.body_hp_db_oct = 6.0;
    p.body = {{600,1.5,5.0},{2200,1.2,3.0}};
    return p;
}
AcousticParams Instruments::violin()
{
    // 弓弦: 稳态供能, 琴箱 A0/B1 共振峰, 弓毛噪声
    AcousticParams p = preset_base("violin", 1.0e-4, 1.2, 5.0, 0.60, 0.006, 0.09);
    p.attack = 0.055; p.sustain = 0.82; p.noise = 0.16; p.noise_len = 0.050;
    p.strike_pos = 0.14; p.strike_p = 1.2;      // 弓位靠近琴马 → 中高频丰富
    p.vel_tilt = 4.0; p.vel_attack = 0.30; p.vel_noise = 0.30;
    p.body = {{275,6.0,6.0},{450,8.0,5.0},{1300,2.0,3.0}};
    p.body_hp_hz = 200; p.body_hp_db_oct = 3.0;
    return p;
}
AcousticParams Instruments::viola()
{
    AcousticParams p = preset_base("viola", 8.0e-5, 1.2, 5.5, 0.55, 0.006, 0.09);
    p.attack = 0.060; p.sustain = 0.84; p.noise = 0.16; p.noise_len = 0.050;
    p.strike_pos = 0.14; p.strike_p = 1.2;
    p.vel_tilt = 4.0; p.vel_attack = 0.30;
    p.body = {{200,6.0,6.0},{400,8.0,5.0},{1100,2.0,3.0}};
    p.body_hp_hz = 180; p.body_hp_db_oct = 3.0;
    return p;
}
AcousticParams Instruments::cello()
{
    AcousticParams p = preset_base("cello", 6.0e-5, 1.2, 5.5, 0.65, 0.006, 0.10);
    p.attack = 0.060; p.sustain = 0.85; p.noise = 0.18; p.noise_len = 0.055;
    p.strike_pos = 0.13; p.strike_p = 1.2;
    p.vel_tilt = 3.5; p.vel_attack = 0.30;
    p.body = {{120,6.0,6.0},{250,8.0,5.0},{800,2.0,3.0}};
    p.body_hp_hz = 100; p.body_hp_db_oct = 3.0;
    return p;
}
AcousticParams Instruments::contrabass()
{
    AcousticParams p = preset_base("contrabass", 5.0e-5, 1.2, 6.0, 0.75, 0.006, 0.10);
    p.attack = 0.070; p.sustain = 0.86; p.noise = 0.18; p.noise_len = 0.060;
    p.strike_pos = 0.13; p.strike_p = 1.2;
    p.vel_tilt = 3.0; p.vel_attack = 0.30;
    p.body = {{80,6.0,6.0},{170,8.0,5.0}};
    p.body_hp_hz = 70; p.body_hp_db_oct = 3.0;
    return p;
}
AcousticParams Instruments::erhu()
{
    // 二胡: 蛇皮共振峰突出 (~300 Hz), 起音含明显弓擦噪声
    AcousticParams p = preset_base("erhu", 1.5e-4, 1.3, 5.0, 0.55, 0.010, 0.09);
    p.attack = 0.070; p.sustain = 0.80; p.noise = 0.28; p.noise_len = 0.060;
    p.strike_pos = 0.12; p.strike_p = 1.2;
    p.vel_tilt = 4.5; p.vel_attack = 0.35; p.vel_noise = 0.35;
    p.body = {{300,8.0,7.0},{900,2.0,4.0},{2000,1.5,-3.0}};
    p.body_hp_hz = 220; p.body_hp_db_oct = 3.0;
    return p;
}
AcousticParams Instruments::flute()
{
    // 长笛: 气柱近纯音, 气流噪声持续, 稳态强
    AcousticParams p = preset_base("flute", 0.0, 0.0, 5.0, 0.40, 0.004, 0.07);
    p.attack = 0.080; p.sustain = 0.90; p.noise = 0.30; p.noise_len = 0.060;
    p.strike_pos = 0.0; p.strike_p = 1.0;       // 气流激励无梳状零点
    p.vel_tilt = 3.0; p.vel_attack = 0.25; p.vel_noise = 0.40;
    p.body = {{2500,1.5,2.0}};
    p.body_hp_hz = 200; p.body_hp_db_oct = 3.0;
    return p;
}
AcousticParams Instruments::dizi()
{
    // 笛子: 比长笛更亮、气流噪声更大 (笛膜)
    AcousticParams p = preset_base("dizi", 0.0, 0.0, 4.5, 0.35, 0.004, 0.06);
    p.attack = 0.060; p.sustain = 0.90; p.noise = 0.38; p.noise_len = 0.050;
    p.vel_tilt = 3.0; p.vel_attack = 0.25; p.vel_noise = 0.45;
    p.body = {{3000,2.0,4.0}};
    p.body_hp_hz = 250; p.body_hp_db_oct = 3.0;
    return p;
}
AcousticParams Instruments::clarinet()
{
    // 单簧管: 闭管 → 只有奇次分音 (激励点放在管中央: pos=0.5 正是这个梳状零点)
    AcousticParams p = preset_base("clarinet", 0.0, 0.0, 4.0, 0.45, 0.004, 0.06);
    p.attack = 0.050; p.sustain = 0.92; p.noise = 0.18; p.noise_len = 0.040;
    p.strike_pos = 0.5; p.strike_p = 1.0;       // 偶次分音 → 0
    p.vel_tilt = 3.0; p.vel_attack = 0.20;
    p.body = {{1500,3.0,5.0}};
    p.body_hp_hz = 120; p.body_hp_db_oct = 3.0;
    return p;
}
AcousticParams Instruments::oboe()
{
    // 双簧管: 簧片尖啸共振 (~1.4 kHz), 中频突出
    AcousticParams p = preset_base("oboe", 0.0, 0.0, 4.0, 0.40, 0.005, 0.06);
    p.attack = 0.050; p.sustain = 0.90; p.noise = 0.20; p.noise_len = 0.040;
    p.strike_pos = 0.10; p.strike_p = 1.0;
    p.vel_tilt = 3.0; p.vel_attack = 0.20;
    p.body = {{1400,3.0,7.0},{3000,2.0,4.0}};
    p.body_hp_hz = 200; p.body_hp_db_oct = 3.0;
    return p;
}
AcousticParams Instruments::sax()
{
    AcousticParams p = preset_base("sax", 0.0, 0.0, 4.5, 0.50, 0.005, 0.07);
    p.attack = 0.050; p.sustain = 0.90; p.noise = 0.26; p.noise_len = 0.045;
    p.strike_pos = 0.12; p.strike_p = 1.0;
    p.vel_tilt = 3.5; p.vel_attack = 0.25; p.vel_noise = 0.35;
    p.body = {{800,2.0,5.0},{2000,1.5,3.0}};
    p.body_hp_hz = 90; p.body_hp_db_oct = 3.0;
    return p;
}
AcousticParams Instruments::trumpet()
{
    // 小号: 铜管, 喇叭口辐射 → 高次分音强
    AcousticParams p = preset_base("trumpet", 0.0, 0.0, 3.5, 0.45, 0.005, 0.06);
    p.attack = 0.040; p.sustain = 0.90; p.noise = 0.22; p.noise_len = 0.030;
    p.strike_pos = 0.10; p.strike_p = 1.0;
    p.vel_tilt = 3.5; p.vel_attack = 0.20; p.vel_noise = 0.35;
    p.body = {{1200,2.0,6.0},{2500,1.5,4.0}};
    p.body_hp_hz = 150; p.body_hp_db_oct = 3.0;
    return p;
}
AcousticParams Instruments::harmonica()
{
    AcousticParams p = preset_base("harmonica", 0.0, 0.0, 4.5, 0.40, 0.005, 0.05);
    p.attack = 0.020; p.sustain = 0.88; p.noise = 0.30; p.noise_len = 0.030;
    p.vel_tilt = 3.0; p.vel_attack = 0.20;
    p.body = {{1000,2.0,3.0}};
    p.body_hp_hz = 180; p.body_hp_db_oct = 3.0;
    return p;
}
AcousticParams Instruments::organ()
{
    // 管风琴: 全稳态长音, 无衰减, 加性谐波固定
    AcousticParams p = preset_base("organ", 0.0, 0.0, 5.0, 6.0, 0.001, 0.05);
    p.attack = 0.045; p.sustain = 1.0; p.noise = 0.08; p.noise_len = 0.060;
    p.body = {{400,1.5,3.0},{1200,1.2,2.0}};
    p.body_hp_hz = 60; p.body_hp_db_oct = 3.0;
    return p;
}
AcousticParams Instruments::bells()
{
    // 钟: 真实的非谐分音列 (哼音 0.5 / 基音 1.0 / 小三度 1.19 / 五度 1.51 / 八度 2.0 ...)
    // 用显式分音表取代"拉伸谐波列", 无制音器 → 自由余音
    AcousticParams p = preset_base("bells", 3.0e-2, 2.0, 0.0, 8.0, 0.0, 0.0);
    p.attack = 0.001; p.noise = 0.25; p.noise_len = 0.004;
    p.partial_table = {{0.50,0.90,12.0},{1.00,1.00,8.0},{1.19,0.62,6.0},
                       {1.51,0.52,5.0},{2.00,0.45,3.5},{2.51,0.30,2.5},
                       {2.66,0.26,2.2},{3.01,0.20,1.8},{4.00,0.14,1.2}};
    p.body = {{1200,1.5,3.0},{3000,1.2,2.0}};
    p.ring = 12.0; p.stereo = 0.35;
    return p;
}
AcousticParams Instruments::gong()
{
    // 锣: 极强非谐 + 极长余音
    AcousticParams p = preset_base("gong", 1.0e-1, 2.0, 0.0, 12.0, 0.0, 0.0);
    p.attack = 0.002; p.noise = 0.35; p.noise_len = 0.020;
    p.partial_table = {{0.50,1.00,15.0},{1.00,0.95,12.0},{1.42,0.70,9.0},
                       {1.73,0.60,8.0},{2.05,0.50,6.0},{2.40,0.40,5.0},
                       {2.90,0.30,4.0},{3.50,0.25,3.0}};
    p.body = {{300,1.5,5.0}};
    p.ring = 20.0; p.stereo = 0.40;
    return p;
}
AcousticParams Instruments::vibraphone()
{
    // 颤音琴: 铝条 1:4:10 分音, 无制音器 → 长余音
    AcousticParams p = preset_base("vibraphone", 4.0e-2, 1.8, 0.0, 3.5, 0.0, 0.0);
    p.attack = 0.002; p.noise = 0.15; p.noise_len = 0.005;
    p.partial_table = {{1.0,1.00,3.5},{4.0,0.35,1.0},{10.0,0.12,0.35}};
    p.body = {{1000,1.2,4.0},{3000,1.0,-3.0}};
    p.ring = 10.0; p.stereo = 0.30;
    return p;
}
AcousticParams Instruments::marimba()
{
    // 马林巴: 木条 1:4:10, 衰减很快
    AcousticParams p = preset_base("marimba", 2.0e-2, 1.6, 0.0, 0.9, 0.0, 0.05);
    p.attack = 0.002; p.noise = 0.25; p.noise_len = 0.004;
    p.partial_table = {{1.0,1.00,0.90},{3.9,0.50,0.30},{9.2,0.20,0.12}};
    p.body = {{800,1.5,4.0},{2400,1.2,-4.0}};
    p.ring = 2.0;
    return p;
}
AcousticParams Instruments::xylophone()
{
    // 木琴: 更高更短更硬
    AcousticParams p = preset_base("xylophone", 3.0e-2, 1.7, 0.0, 0.6, 0.0, 0.05);
    p.attack = 0.001; p.noise = 0.30; p.noise_len = 0.003;
    p.partial_table = {{1.0,1.00,0.60},{3.0,0.60,0.20},{6.3,0.25,0.10}};
    p.body = {{1500,1.5,5.0},{3500,1.2,-3.0}};
    p.ring = 1.5;
    return p;
}
AcousticParams Instruments::timpani()
{
    // 定音鼓: 膜振动分音 ≈ 1:1.5:1.85:2.3, 有制音器(手/毡)
    AcousticParams p = preset_base("timpani", 2.0e-2, 1.8, 0.0, 1.6, 0.0, 0.15);
    p.attack = 0.002; p.noise = 0.30; p.noise_len = 0.010;
    p.partial_table = {{1.0,1.00,1.6},{1.5,0.60,1.2},{1.85,0.40,0.9},
                       {2.3,0.25,0.6},{2.9,0.15,0.4}};
    p.body = {{100,1.2,6.0}};
    p.ring = 3.0;
    return p;
}
AcousticParams Instruments::taiko()
{
    // 太鼓: 低频膜音 + 强击打噪声
    AcousticParams p = preset_base("taiko", 3.0e-2, 1.8, 0.0, 1.0, 0.0, 0.20);
    p.attack = 0.002; p.noise = 0.50; p.noise_len = 0.020;
    p.partial_table = {{1.0,1.00,1.0},{1.6,0.50,0.6},{2.4,0.30,0.4},
                       {3.4,0.15,0.25},{4.5,0.10,0.15}};
    p.body = {{80,1.2,6.0}};
    p.ring = 2.0;
    return p;
}
AcousticParams Instruments::choir()
{
    // 人声合唱: 共振峰在 500 Hz / 1.2 kHz (元音", a"), 起音慢
    AcousticParams p = preset_base("choir", 0.0, 0.0, 6.0, 3.0, 0.010, 0.25);
    p.attack = 0.120; p.sustain = 0.85; p.noise = 0.10; p.noise_len = 0.080;
    p.unison = 3; p.detune_cents = 0.6;         // 多人轻微失谐 → 合唱感
    p.body = {{500,1.5,5.0},{1200,1.5,3.0}};
    p.body_hp_hz = 120; p.body_hp_db_oct = 3.0;
    return p;
}
AcousticParams Instruments::synth_pad()
{
    // 合成铺底: 慢起音、多路失谐、随机初相
    AcousticParams p = preset_base("synth_pad", 0.0, 0.0, 7.0, 3.0, 0.010, 0.30);
    p.attack = 0.120; p.sustain = 0.85; p.noise = 0.05; p.noise_len = 0.100;
    p.unison = 3; p.detune_cents = 4.0;
    p.phase = PhaseMode::Random;
    p.body = {{1000,1.0,2.0}};
    p.body_hp_hz = 80; p.body_hp_db_oct = 3.0;
    return p;
}

const std::vector<AcousticParams>& Instruments::all()
{
    static const std::vector<AcousticParams> list = {
        // 键盘
        piano(), e_piano(), harpsichord(), music_box(),
        // 拨弦
        guitar(), guitar_nylon(), bass_guitar(), harp(),
        guzheng(), pipa(), yangqin(),
        // 弓弦
        violin(), viola(), cello(), contrabass(), erhu(),
        // 管乐
        flute(), dizi(), clarinet(), oboe(), sax(), trumpet(), harmonica(), organ(),
        // 打击 / 金属
        bells(), gong(), vibraphone(), marimba(), xylophone(), timpani(), taiko(),
        // 电声 / 人声
        choir(), synth_pad(),
    };
    return list;
}

const AcousticParams* Instruments::find_by_name(std::string_view name)
{
    std::string want = lower(name);
    for (const auto& p : all())
        if (lower(p.name) == want)
            return &p;
    return nullptr;
}

std::string Instruments::category_of(std::string_view name)
{
    const std::string n = lower(name);
    auto in = [&](std::initializer_list<const char*> xs) {
        for (const char* x : xs) if (n == x) return true;
        return false;
    };
    if (in({"piano","e_piano","harpsichord","music_box"}))              return "键盘";
    if (in({"guitar","guitar_nylon","bass_guitar","harp",
            "guzheng","pipa","yangqin"}))                               return "拨弦";
    if (in({"violin","viola","cello","contrabass","erhu"}))             return "弓弦";
    if (in({"flute","dizi","clarinet","oboe","sax","trumpet",
            "harmonica","organ"}))                                      return "管乐";
    if (in({"bells","gong","vibraphone","marimba","xylophone",
            "timpani","taiko"}))                                        return "打击";
    if (in({"choir","synth_pad"}))                                      return "电声/人声";
    return "其他";
}

// ── 旧语法: 谐波行 / 持续比例行 ───────────────────────────────────
bool is_harmonics_line(std::string_view line)
{
    std::string t = trim_ws(line);
    if (t.empty()) return false;

    // 精确判据: 整行必须是"纯数字 + 逗号", 且至少 2 个数字。
    // 这样 1,0.7,0.5,0.3 这类音色行能被识别, 而带 '.'/空格/'_'/'!'/'$'/'@'/'[' 
    // 的音符行(简谱音符必然含 '.' 分隔音级与时值)绝不会被误判。
    if (t.find_first_of(" \t[]_!$@") != std::string::npos) return false;

    std::stringstream ss(t);
    std::string tok;
    size_t n = 0;
    while (std::getline(ss, tok, ',')) {
        std::string s = trim_ws(tok);
        if (s.empty()) return false;
        char* end = nullptr;
        double v = std::strtod(s.c_str(), &end);
        if (end == s.c_str() || *end != '\0' || v < 0.0) return false;
        ++n;
    }
    return n >= 2;
}

std::vector<double> parse_harmonics_line(std::string_view line)
{
    std::vector<double> h;
    std::stringstream ss{trim_ws(line)};
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        std::string t = trim_ws(tok);
        if (t.empty()) continue;
        h.push_back(parse_double(t, "harmonic amplitude"));
    }
    return h;
}

namespace {
SustainRegion parse_region(std::string_view tok)
{
    bool fade = false;
    if (!tok.empty() && tok.back() == '>') {
        fade = true;
        tok = tok.substr(0, tok.size() - 1);
    }
    auto dash = tok.find('-');
    if (dash == std::string_view::npos)
        fail("sustain region missing '-': " + std::string(tok));
    double start = parse_double(tok.substr(0, dash), "sustain start");
    double end   = parse_double(tok.substr(dash + 1), "sustain end");
    if (!(start >= 0.0 && end <= 1.0 && start <= end))
        fail("sustain region out of range [0,1]: " + std::string(tok));
    return {start, end, fade};
}
} // namespace

std::vector<HarmonicSustain> parse_sustain_line(std::string_view line)
{
    std::vector<HarmonicSustain> result;
    std::stringstream ss{trim_ws(line)};
    std::string token;
    while (std::getline(ss, token, '!')) {
        std::string t = trim_ws(token);
        if (t.empty()) continue;

        HarmonicSustain hs;
        if (t.front() == '{') {
            if (t.back() != '}')
                fail("unbalanced '{' in sustain: " + t);
            std::string inner = t.substr(1, t.size() - 2);
            std::stringstream is(inner);
            std::string region;
            while (std::getline(is, region, ',')) {
                std::string r = trim_ws(region);
                if (r.empty()) continue;
                hs.regions.push_back(parse_region(r));
            }
            if (hs.regions.empty())
                fail("empty sustain group: " + t);
        } else {
            hs.regions.push_back(parse_region(t));
        }
        result.push_back(std::move(hs));
    }
    return result;
}

bool is_sustain_line(std::string_view line)
{
    try {
        return !parse_sustain_line(line).empty();
    } catch (const std::exception&) {
        return false;
    }
}

// ── 旧接口: 头部解析 ──────────────────────────────────────────────
bool parse_rcp_header(std::string_view line,
                      double& bpm,
                      double& base_freq,
                      double& base_beat_duration)
{
    std::string s = trim_ws(line);
    if (s.empty()) return false;
    std::vector<double> vals;
    std::stringstream ss(s);
    std::string token;
    while (std::getline(ss, token, ',')) {
        std::string t = trim_ws(token);
        if (t.empty()) return false;
        char* end = nullptr;
        double v = std::strtod(t.c_str(), &end);
        if (end == t.c_str() || *end != '\0') return false;
        vals.push_back(v);
    }
    if (vals.size() != 3) return false;
    if (vals[0] <= 0.0 || vals[1] <= 0.0 || vals[2] <= 0.0) return false;
    bpm = vals[0];
    base_freq = vals[1];
    base_beat_duration = vals[2];
    return true;
}

// ── token 分词 (识别 [...] 分组) ──────────────────────────────────
std::vector<std::string> split_note_tokens(std::string_view line)
{
    std::vector<std::string> out;
    int depth = 0;
    std::string cur;
    for (char c : line) {
        if (c == '[') { ++depth; cur.push_back(c); continue; }
        if (c == ']') { depth = std::max(0, depth - 1); cur.push_back(c); continue; }
        bool ws = (c == ' ' || c == '\t' || c == '\r' || c == '\n');
        if (ws && depth == 0) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool looks_like_note_line(std::string_view line)
{
    auto toks = split_note_tokens(line);
    if (toks.empty()) return false;
    NoteParser probe;
    for (const auto& t : toks) {
        try {
            (void)probe.parse_token(t);
        } catch (const std::exception&) {
            return false;
        }
    }
    return true;
}

bool is_directive_line(std::string_view line)
{
    std::string t = trim_ws(line);
    return !t.empty() && t[0] == '@';
}

/// 去掉注释: '#' 需位于行首且后接空白, 或位于空白之后
/// 音符的升降号 "#4" 紧贴音级、前面无空白, 不会被误伤
std::string strip_comment(std::string_view line)
{
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] != '#') continue;
        bool at_line_start = (i == 0);
        bool after_space = (i > 0 && (line[i - 1] == ' ' || line[i - 1] == '\t'));
        if (!at_line_start && !after_space) continue;
        // 要求 '#' 之后是空白或行尾, 避免把 "#4" 这类升号当成注释
        if (i + 1 < line.size() && line[i + 1] != ' ' && line[i + 1] != '\t') continue;
        size_t e = i;
        while (e > 0 && (line[e - 1] == ' ' || line[e - 1] == '\t'))
            --e;
        return std::string(line.substr(0, e));
    }
    return std::string(line);
}

std::vector<std::string> tokenize_notes(const std::vector<std::string>& lines)
{
    std::vector<std::string> notes;
    for (const auto& line : lines) {
        for (auto& t : split_note_tokens(line))
            notes.push_back(std::move(t));
    }
    return notes;
}

// ── 音名 → 频率 (@ref A4=440 / @ref C4 / @ref 261.63) ────────────
namespace {
double semitone_of_letter(char c)
{
    switch (std::tolower(static_cast<unsigned char>(c))) {
        case 'c': return 0;
        case 'd': return 2;
        case 'e': return 4;
        case 'f': return 5;
        case 'g': return 7;
        case 'a': return 9;
        case 'b': return 11;
        default:  return -1;
    }
}

/// 解析 "261.63" / "A4=440" / "C4" / "F#3" / "bB3"
/// 返回 0 表示不是音名写法 (由调用方按纯数字处理)
double parse_pitch_ref(std::string_view sv)
{
    std::string s = trim_ws(sv);
    if (s.empty()) return -1.0;

    // 纯数字
    {
        char* end = nullptr;
        double v = std::strtod(s.c_str(), &end);
        if (end != s.c_str() && *end == '\0')
            return v;
    }

    // NAME=HZ
    auto eq = s.find('=');
    double a4 = 440.0;
    std::string note = s;
    if (eq != std::string::npos) {
        note = trim_ws(std::string_view(s).substr(0, eq));
        a4 = parse_double(std::string_view(s).substr(eq + 1), "@ref value");
    }

    // 去掉可能的 '1' / 'do' 前缀 (简谱写法 1=C)
    while (!note.empty() && (note.front() == '1' || note.front() == 'd' || note.front() == 'D')) {
        if (note.size() >= 2 && (note[0] == 'd' || note[0] == 'D') && (note[1] == 'o' || note[1] == 'O'))
            note = trim_ws(std::string_view(note).substr(2));
        else if (note.front() == '1' || (note.front() == 'd' && note.size() > 1 && std::isdigit(static_cast<unsigned char>(note[1]))))
            note = trim_ws(std::string_view(note).substr(1));
        else
            break;
        auto e2 = note.find('=');
        if (e2 != std::string::npos) note = trim_ws(std::string_view(note).substr(e2 + 1));
    }
    if (note.empty()) throw std::runtime_error("bad pitch reference: " + s);

    size_t i = 0;
    if (note[i] == 'b' && note.size() > 1 && semitone_of_letter(note[1]) >= 0) ++i;  // bB3
    double semi = semitone_of_letter(note[i]);
    if (semi < 0) throw std::runtime_error("bad pitch name: " + s);
    ++i;
    while (i < note.size() && (note[i] == '#' || note[i] == 'b')) {
        semi += (note[i] == '#') ? 1.0 : -1.0;
        ++i;
    }
    int octave = 4;
    if (i < note.size()) {
        std::string oct = note.substr(i);
        char* end = nullptr;
        long v = std::strtol(oct.c_str(), &end, 10);
        if (end == oct.c_str() || *end != '\0')
            throw std::runtime_error("bad octave in pitch name: " + s);
        octave = static_cast<int>(v);
    }
    double midi = 12.0 * (octave + 1) + semi;
    return a4 * std::pow(2.0, (midi - 69.0) / 12.0);
}
} // namespace

// ── RCP 文档解析 ──────────────────────────────────────────────────
static RcpDocument parse_rcp_strict_impl(std::string_view content, bool strict);

RcpDocument parse_rcp(std::string_view content)
{
    return parse_rcp_strict_impl(content, false);
}

RcpDocument parse_rcp_strict(std::string_view content)
{
    return parse_rcp_strict_impl(content, true);
}

static RcpDocument parse_rcp_strict_impl(std::string_view content, bool strict)
{
    RcpDocument doc;

    // 去 UTF-8 BOM
    if (content.size() >= 3 &&
        static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF)
        content.remove_prefix(3);

    // 拆行 (去掉注释, 丢弃空行)
    std::vector<std::string> raw;
    {
        std::stringstream ss{std::string(content)};
        std::string line;
        while (std::getline(ss, line)) {
            std::string t = trim_ws(strip_comment(line));
            if (!t.empty()) raw.push_back(std::move(t));
        }
    }

    // 预扫描 @dyn: 自定义力度表必须先于音符生效 (指令行的书写顺序无关)
    for (const auto& l : raw) {
        if (!is_directive_line(l)) continue;
        std::stringstream ls(l.substr(1));
        std::string dkey;
        ls >> dkey;
        if (lower(dkey) != "dyn") continue;
        std::string drest;
        std::getline(ls, drest);
        apply_dyn_directive(drest, doc);
    }

    // 头部: 第一行必须是 "BPM,ref,beat" 或含 @bpm/@ref/@beat 的行
    size_t i = 0;
    while (i < raw.size() && raw[i].empty()) ++i;
    if (i >= raw.size())
        throw std::runtime_error("RCP content is empty");

    bool header_set = false;
    {
        const std::string& first = raw[i];
        if (is_directive_line(first)) {
            // 允许第一行直接是 @ 指令, 头部参数后续用 @bpm/@ref/@beat 给出
            header_set = false;
        } else {
            double bpm = 0, ref = 0, beat = 0;
            if (!parse_rcp_header(first, bpm, ref, beat))
                throw std::runtime_error("Invalid RCP header: " + first);
            doc.bpm = bpm;
            doc.base_freq = ref;
            doc.beat_duration = beat;
            header_set = true;
            // BPM 与拍长自洽性检查
            double implied = 60.0 / bpm;
            if (std::abs(implied - beat) > 0.01 * std::max(implied, beat)) {
                std::ostringstream w;
                w << "头部 BPM=" << bpm << " 与标准拍长=" << beat
                  << "s 不一致 (60/BPM=" << implied
                  << "s), 时值以拍长为准, 隐含 BPM=" << (60.0 / beat);
                doc.warnings.push_back(w.str());
            }
            ++i;
        }
        (void)header_set;
    }

    // 预先扫描: 无前缀的旧式第 2/3 行如何识别
    // 需要知道"当前行往后"哪些是音符行, 才能避免把音符当成音色行
    auto legacy_harmonics_at = [&](size_t idx) -> bool {
        if (idx >= raw.size()) return false;
        const std::string& l = raw[idx];
        if (l.empty() || is_directive_line(l)) return false;
        if (!is_harmonics_line(l)) return false;
        return !looks_like_note_line(l);   // 是合法音符行就不当音色行
    };

    NoteParser parser(doc.base_freq, doc.beat_duration);
    parser.set_dyn_table(&doc.dyn);
    bool legacy_phase = true;

    for (; i < raw.size(); ++i) {
        const std::string& line = raw[i];
        if (line.empty()) continue;

        if (is_directive_line(line)) {
            legacy_phase = false;   // 出现 @ 之后不再做无前缀嗅探
            std::string body = line.substr(1);
            std::stringstream ls(body);
            std::string key;
            ls >> key;
            std::string rest;
            std::getline(ls, rest);
            key = lower(key);

            if (key == "bpm" || key == "tempo") {
                doc.bpm = parse_double(rest, "@bpm");
            } else if (key == "ref" || key == "freq" || key == "base" || key == "key") {
                // 支持 261.63 / C4 / A4=440 / 1=C 四种写法
                doc.base_freq = parse_pitch_ref(rest);
                if (!(doc.base_freq > 0.0))
                    fail("bad @ref value: " + trim_ws(rest));
                parser.set_base_freq(doc.base_freq);
            } else if (key == "beat") {
                doc.beat_duration = parse_double(rest, "@beat");
                parser.set_base_beat_duration(doc.beat_duration);
            } else if (key == "meter") {
                std::string m = trim_ws(rest);
                auto slash = m.find('/');
                if (slash == std::string::npos)
                    fail("bad @meter (need N/D): " + m);
                doc.meter_num = static_cast<int>(parse_double(std::string_view(m).substr(0, slash), "@meter"));
                doc.meter_den = static_cast<int>(parse_double(std::string_view(m).substr(slash + 1), "@meter"));
                if (doc.meter_num <= 0 || doc.meter_den <= 0)
                    fail("bad @meter values: " + m);
            } else if (key == "timbre" || key == "instrument") {
                std::string name = lower(trim_ws(rest));
                if (name.empty()) fail("@timbre needs a name");
                doc.timbre_name = name;
                if (const AcousticParams* p = Instruments::find_by_name(name)) {
                    doc.acoustic = *p;
                    doc.has_acoustic = true;
                }
            } else if (key == "acoustic") {
                std::string r = trim_ws(rest);
                auto sp = r.find_first_of(" \t");
                std::string kv;
                if (sp == std::string::npos) {
                    // 只有名称: 取预设
                    if (const AcousticParams* p = Instruments::find_by_name(r)) {
                        doc.acoustic = *p;
                        doc.has_acoustic = true;
                    } else {
                        throw std::runtime_error("unknown instrument in @acoustic: " + r);
                    }
                } else {
                    std::string name = lower(trim_ws(std::string_view(r).substr(0, sp)));
                    kv = trim_ws(std::string_view(r).substr(sp + 1));
                    if (const AcousticParams* p = Instruments::find_by_name(name))
                        doc.acoustic = *p;                 // 预设打底
                    else
                        doc.acoustic = AcousticParams{};   // 纯自定义
                    doc.acoustic.name = name;
                    AcousticParams ov = parse_acoustic_params(kv);
                    merge_acoustic(doc.acoustic, ov);      // 只覆盖显式给出的键
                    doc.has_acoustic = true;
                }
            } else if (key == "dyn") {
                apply_dyn_directive(rest, doc);
            } else if (key == "voice" || key == "v") {
                // 多声部标记: 事件纵向叠加, 无需特殊处理
            } else {
                doc.warnings.push_back("未知指令 @" + key + " (已忽略)");
            }
            continue;
        }

        // 无前缀行: 旧式第 2/3 行嗅探
        if (legacy_phase) {
            if (!doc.has_harmonics && legacy_harmonics_at(i)) {
                doc.harmonics = parse_harmonics_line(line);
                doc.has_harmonics = true;
                continue;
            }
            if (doc.has_harmonics && !doc.has_sustain && is_sustain_line(line)
                && !looks_like_note_line(line)) {
                doc.sustain = parse_sustain_line(line);
                doc.has_sustain = true;
                doc.warnings.push_back(
                    "检测到旧式持续比例行: 时间窗以音符时长比例解释, 建议改用 @acoustic");
                continue;
            }
            legacy_phase = false;
        }

        // 音符行
        // 容错策略: 单个 token 写错时, 记录"第几行 / 哪个 token / 原因"并跳过它,
        // 其余音符照常演奏 —— 一份几百行的谱不该因为一个笔误就整体打不开。
        // 需要严格模式(如自动化校验)请用 parse_rcp_strict。
        for (const auto& tok : split_note_tokens(line)) {
            std::vector<Note> notes;
            try {
                notes = parser.parse_token(tok);
            } catch (const std::exception& e) {
                if (strict)
                    throw std::invalid_argument("第 " + std::to_string(i + 1)
                        + " 行 token \"" + tok + "\": " + e.what());
                std::ostringstream w;
                w << "第 " << (i + 1) << " 行 token \"" << tok << "\" 已跳过: " << e.what();
                doc.warnings.push_back(w.str());
                continue;
            }
            bool first = true;
            for (auto& n : notes) {
                n.token_start = first;
                first = false;
                doc.notes.push_back(std::move(n));
            }
        }
    }

    if (doc.notes.empty())
        doc.warnings.push_back("未解析到任何音符");

    return doc;
}
