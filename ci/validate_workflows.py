import glob
import sys
try:
    import yaml
except ImportError:
    print("PyYAML 未安装, 跳过严格解析")
    sys.exit(3)

# 校验 .github/workflows 下实际存在的全部 workflow (增删 workflow 无需改这个脚本)
files = sorted(glob.glob(".github/workflows/*.yml") + glob.glob(".github/workflows/*.yaml"))
if not files:
    print("FAIL: .github/workflows 下没有找到任何 workflow")
    sys.exit(1)
ok = True
for f in files:
    try:
        with open(f, encoding="utf-8") as fh:
            d = yaml.safe_load(fh)
        on = d.get(True, d.get("on"))
        print("OK   %s" % f)
        print("     name=%s" % d.get("name"))
        print("     jobs=%s" % list((d.get("jobs") or {}).keys()))
        print("     on=%s" % (list(on.keys()) if isinstance(on, dict) else on))
        # 复用 job 的 uses 路径必须存在
        for jn, jd in (d.get("jobs") or {}).items():
            if "uses" in jd and jd["uses"].startswith("./"):
                import os
                p = jd["uses"][2:]
                if not os.path.exists(p):
                    print("     FAIL: job %s uses 不存在的 %s" % (jn, p))
                    ok = False
    except Exception as e:
        ok = False
        print("FAIL %s: %s" % (f, e))
sys.exit(0 if ok else 1)
