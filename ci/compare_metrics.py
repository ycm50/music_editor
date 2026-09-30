#!/usr/bin/env python3
"""按字段比较 save --metrics 的输出 (跨平台合成一致性闸门)。

为什么要容差比较: --metrics 里的计数类字段 (events/samples/chords/...) 必须完全一致,
但音频统计量 (rms/crest/silent/...) 经过几百万次浮点运算, 不同架构的 libm 与指令
(尤其 aarch64 默认启用 FMA) 会让尾数在 1e-15 量级漂移 —— 一旦样本落在 1e-4 这类硬阈值
附近, 静音样本数这种计数就会差几个。用逐字段容差代替 diff 才能既拦住真正的算法改动,
又不被平台噪声误伤。

用法: compare_metrics.py <期望文件> <实际文件>
退出码: 0 = 全部在容差内; 1 = 有字段超差; 2 = 文件结构不一致
"""
import sys
from collections import OrderedDict

# 计数/整型字段: 必须完全相同
EXACT = {"events", "samples", "discarded", "chords", "poly", "trunc", "glitch",
         "pmin", "pmax"}

# 浮点字段: (相对容差, 绝对容差)
TOL = {
    "dur":    (1e-6, 1e-6),
    "peak":   (1e-4, 1e-4),
    "rms":    (1e-4, 1e-5),
    "dc":     (0.0,  1e-6),   # 直流判据是绝对值, 相对容差无意义
    "crest":  (1e-3, 1e-2),   # dB
    "clip":   (0.0,  1e-6),
    "silent": (0.0,  5e-4),   # 占比: 允许万分之五的样本在硬阈值附近抖动
    "fit":    (0.0,  1e-3),
    "fmin":   (1e-5, 1e-3),
    "fmax":   (1e-5, 1e-3),
    "tp":     (1e-4, 1e-4),
    "jump":   (1e-2, 1e-2),
    "crmin":  (1e-3, 1e-3),
    "crmax":  (1e-3, 1e-3),
}
DEFAULT_TOL = (1e-4, 1e-4)


def parse(path):
    rows = []
    with open(path, encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            name, fields = parts[0], OrderedDict()
            for item in parts[1:]:
                if "=" in item:
                    k, v = item.split("=", 1)
                    fields[k] = v
            rows.append((name, fields))
    return rows


def main(argv):
    if len(argv) != 3:
        print("用法: compare_metrics.py <期望文件> <实际文件>", file=sys.stderr)
        return 2
    exp, act = parse(argv[1]), parse(argv[2])

    if [r[0] for r in exp] != [r[0] for r in act]:
        print("乐谱条目不一致:", file=sys.stderr)
        print("  期望:", [r[0] for r in exp], file=sys.stderr)
        print("  实际:", [r[0] for r in act], file=sys.stderr)
        return 2

    bad = 0
    print("%-28s %-9s %-14s %-14s %s" % ("乐谱", "字段", "期望", "实际", "偏差/容差"))
    for (n_exp, f_exp), (_, f_act) in zip(exp, act):
        keys = list(f_exp.keys()) + [k for k in f_act if k not in f_exp]
        for k in keys:
            v_exp, v_act = f_exp.get(k), f_act.get(k)
            if v_exp is None or v_act is None:
                print("%-28s %-9s %-14s %-14s 字段缺失" % (n_exp, k, v_exp, v_act))
                bad += 1
                continue
            if k in EXACT:
                if v_exp != v_act:
                    print("%-28s %-9s %-14s %-14s 计数必须一致" % (n_exp, k, v_exp, v_act))
                    bad += 1
                continue
            try:
                a, b = float(v_exp), float(v_act)
            except ValueError:
                if v_exp != v_act:
                    print("%-28s %-9s %-14s %-14s 非数值不一致" % (n_exp, k, v_exp, v_act))
                    bad += 1
                continue
            rel_tol, abs_tol = TOL.get(k, DEFAULT_TOL)
            diff = abs(a - b)
            if diff <= abs_tol:
                continue
            if rel_tol > 0.0 and a != 0.0 and diff / abs(a) <= rel_tol:
                continue
            rel = (diff / abs(a)) if a else float("inf")
            print("%-28s %-9s %-14s %-14s 超差 (rel %.2e / abs %.2e; 容差 rel %.0e / abs %.0e)"
                  % (n_exp, k, v_exp, v_act, rel, diff, rel_tol, abs_tol))
            bad += 1

    if bad:
        print("", file=sys.stderr)
        print("合成一致性: 与基准不一致 (超差 %d 个字段)" % bad, file=sys.stderr)
        print("若确实改了合成算法/乐器参数, 请同步更新基准:", file=sys.stderr)
        print("  for d in demos/*.rcp; do ./build/save/save \"$d\" --metrics \\", file=sys.stderr)
        print("      | sed 's|^METRICS ||; s|\\\\|/|g; s|[^ ]*/||'; done > ci/metrics_golden.txt",
              file=sys.stderr)
        return 1
    print("合成一致性: 与基准一致 (%d 条; 计数精确 + 浮点容差)" % len(exp))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
