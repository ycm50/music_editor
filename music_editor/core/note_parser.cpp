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

/// 去掉 token 上 "连音/物理/力度" 之后的所有修饰, 返回主干 "音.八度分母[修饰]"
/// 同时把修饰解析进 Note。
void parse_modifiers(std::string_view tail, Note& n)
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
                n.velocity = v;
            } else if (name == "pp")  n.velocity = 0.15;
            else if (name == "p")     n.velocity = 0.30;
            else if (name == "mp")    n.velocity = 0.45;
            else if (name == "mf")    n.velocity = 0.60;
            else if (name == "f")     n.velocity = 0.78;
            else if (name == "ff")    n.velocity = 1.00;
            else fail("unknown dynamic: !" + name);
            if (n.velocity < 0.0 || n.velocity > 1.0)
                fail("dynamic out of range [0,1]: " + name);
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
    // 连音符: 't' = 三连音 (×3/2), 'q' = 五连音 (×5/4)
    if (!tail.empty() && tail[0] == 't') { denom /= 1.5; tail.remove_prefix(1); }
    else if (!tail.empty() && tail[0] == 'q') { denom /= 1.25; tail.remove_prefix(1); }

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
    parse_modifiers(tail_v, n);

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
        std::string cur;
        for (size_t i = 0; i < inner.size(); ++i) {
            const char c = inner[i];
            if (c == ',') { members.push_back(cur); cur.clear(); continue; }
            if (c == '+') {
                // 八度记号 '+' 紧跟在 '.' 之后; 其余位置的 '+' 视为和弦成员分隔符
                const bool is_octave_sign = !cur.empty() && cur.back() == '.';
                if (!is_octave_sign) {
                    if (!cur.empty()) {
                        members.push_back(cur);
                        cur.clear();
                        continue;
                    }
                }
            }
            cur.push_back(c);
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
            parse_modifiers(tail, n);
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
    base.set_mask |= o.set_mask;
}

// ── 动态映射解析 (@dyn mf=0.6,p=0.3) ─────────────────────────────
namespace {
void apply_dyn_directive(std::string_view arg, RcpDocument& doc)
{
    (void)doc;
    std::stringstream ss{trim_ws(arg)};
    std::string item;
    while (std::getline(ss, item, ',')) {
        std::string it = trim_ws(item);
        if (it.empty()) continue;
        auto eq = it.find('=');
        if (eq == std::string::npos)
            fail("bad @dyn item (need name=value): " + it);
        lower(trim_ws(std::string_view(it).substr(0, eq)));
        parse_double(std::string_view(it).substr(eq + 1), "@dyn value");
        // 动态表当前由解析器内建; 自定义映射保留给后续版本, 此处只做语法校验
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
    return p;
}

// ── 物理预设 ─────────────────────────────────────────────────────
namespace {
AcousticParams make(const char* name, double B, double decay, double exp,
                    double atk, double sus, double rel, double br,
                    double noise, double stereo,
                    std::initializer_list<double> pd = {})
{
    AcousticParams p;
    p.name = name;
    p.inharmonicity = B;
    p.decay = decay;
    p.damping_exp = exp;
    p.attack = atk;
    p.sustain = sus;
    p.release = rel;
    p.brightness = br;
    p.noise = noise;
    p.stereo = stereo;
    p.partial_decay.assign(pd.begin(), pd.end());
    return p;
}
} // namespace

AcousticParams Instruments::piano()
{
    // 中音区实测 B≈2e-4; 弦越粗劲度越显著, 逐分音 tau 下降低音区更慢
    return make("piano", 2.0e-4, 2.45, 0.75, 0.003, 0.0, 0.05, 0.68, 0.22, 0.15);
}
AcousticParams Instruments::violin()
{
    // 弓持续供能 → 稳态; 起音含弓毛摩擦噪声; 中高频丰富
    return make("violin", 1.0e-4, 0.55, 0.55, 0.055, 0.82, 0.09, 0.88, 0.16, 0.30);
}
AcousticParams Instruments::flute()
{
    // 气流为主 → 极纯; 起音 0.08s; 稳态强; 高次谐波迅速衰减
    return make("flute", 0.0, 0.35, 0.90, 0.080, 0.90, 0.07, 0.30, 0.30, 0.10);
}
AcousticParams Instruments::guitar()
{
    // 拨弦: 亮起音 + 快速衰减; B 比钢琴小
    return make("guitar", 1.0e-5, 1.10, 0.95, 0.002, 0.0, 0.04, 0.78, 0.35, 0.12);
}
AcousticParams Instruments::harp()
{
    // 竖琴: 比吉他更长的衰减, 更亮更纯净
    return make("harp", 8.0e-5, 2.20, 0.90, 0.003, 0.0, 0.05, 0.80, 0.20, 0.25);
}
AcousticParams Instruments::bells()
{
    // 钟: 强非谐性; f_n 严格非整数比(此处用大 B 近似拍频感); 长衰减
    return make("bells", 3.0e-2, 4.50, 0.45, 0.001, 0.0, 0.06, 0.62, 0.25, 0.35);
}
AcousticParams Instruments::music_box()
{
    // 八音盒: 金属齿, 快起音, 清脆, 中短衰减
    return make("music_box", 6.0e-3, 1.60, 0.85, 0.001, 0.0, 0.03, 0.86, 0.30, 0.20);
}
AcousticParams Instruments::organ()
{
    // 管风琴: 稳态长音, 无衰减, 加性谐波固定
    return make("organ", 0.0, 6.00, 0.20, 0.045, 1.0, 0.10, 0.75, 0.08, 0.20);
}

const std::vector<AcousticParams>& Instruments::all()
{
    static const std::vector<AcousticParams> list = {
        piano(), violin(), flute(), guitar(),
        harp(),  bells(),  music_box(), organ(),
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
