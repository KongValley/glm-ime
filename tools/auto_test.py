#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""glm-ime 全自动 E2E 测试 v2（单进程 suite 模式，避免反复前台竞争）。
用例格式: name||keys||expect；host 一次启动跑全部，Python 断言。
用法: python tools/auto_test.py
"""
import io, os, struct, subprocess, sys, tempfile, time
from pathlib import Path

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
ROOT = Path(__file__).resolve().parent.parent
HOST = ROOT / "tools" / "e2e_host" / "build" / "Release" / "e2e_host.exe"
WORK = Path(tempfile.mkdtemp(prefix="glm_suite_"))

CASES = [
    # name          keys                                    expect
    ("basic",       "nihao<SP>",                            "你好"),
    ("repeat3",     "nihao<SP>nihao<SP>nihao<SP>",          "你好你好你好"),
    ("split",       "ni<SP>hao<SP>",                        "你好"),
    ("enter_raw",   "vibe<ENT>",                            "vibe"),
    ("esc_cancel",  "nihao<ESC>",                           ""),
    ("backspace",   "nihao<BS><BS><BS><BS><BS><SP>",        ""),
    ("digit1",      "nihao1",                               "你好"),
    ("nomatch",     "zzzz<SP>",                             "zzzz"),
    ("sentence",    "zhongguo<SP>gongzuo<SP>",              "中国工作"),
    ("theme_dark",  "nihao<SHOT><SP>",                      "你好"),
    ("theme_multi", "c<SHOT><ESC>",                         ""),
    ("csssss_basic", "csssss<SP>",                          "csssss"),
    ("csssss_stages", "c<SHOT>s<SHOT>ssss<SHOT><SP>",       "csssss"),
]

def read_bmp(path):
    data = open(path, "rb").read()
    off = struct.unpack_from("<I", data, 10)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    h = abs(h)
    px = []
    stride = w * 4
    for y in range(h):
        row = data[off + y*stride: off + y*stride + stride]
        px.append([(row[x*4+2], row[x*4+1], row[x*4+0]) for x in range(w)])
    return w, h, px

def shot_stats(path):
    """内部区域采样（去掉边缘 6px，排除 BitBlt 含窗口外背景的假阳性）"""
    w, h, px = read_bmp(path)
    inner = [p for y in range(6, max(7, h-6)) for p in px[y][6:max(7, w-6)]]
    whites = sum(1 for (r, g, b) in inner if r > 200 and g > 200 and b > 200)
    lum = sum(sum(p)/3 for p in inner) / max(1, len(inner))
    dark = sum(1 for (r, g, b) in inner if (r+g+b)/3 < 90)
    return w, h, whites/max(1,len(inner)), lum, dark/max(1,len(inner))

if __name__ == "__main__":
    if not HOST.exists():
        print("host missing"); sys.exit(2)
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    "Get-Process e2e_host -ErrorAction SilentlyContinue | Stop-Process -Force"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    suite = WORK / "suite.txt"
    suite.write_text("\n".join(f"{n}||{k}||{e}" for n, k, e in CASES), encoding="utf-8")
    out = WORK / "result.txt"
    # 非侵入约定（docs/TECH.md §6.3）：前台被占用时不硬抢；单次重试后报告环境不可用
    focus_ok = False
    for attempt in range(2):
        if out.exists(): out.unlink()
        subprocess.run([str(HOST), "--suite", str(suite), "--out", str(out)],
                       cwd=str(HOST.parent), timeout=300,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if out.exists():
            lines = out.read_text(encoding="utf-8", errors="replace").splitlines()
            if lines and lines[0].startswith("FOCUS|"):
                focus_ok = lines[0].split("|")[1] == "1"
                if focus_ok: break
            if any(l.startswith("ENV_UNAVAILABLE") for l in lines):
                if attempt == 0:
                    print("  (环境忙，5 秒后重试一次…)")
                    time.sleep(5)
                    continue
                print("\n环境不可用：前台窗口被其它应用占用（当前不适合自动化测试）。")
                print("请在方便时（不操作键盘鼠标的窗口期）重新运行：python tools/auto_test.py")
                sys.exit(3)
    if not out.exists():
        print("NO RESULT FILE"); sys.exit(2)

    results = []
    focus = 1
    for line in out.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("FOCUS|"):
            focus = int(line[6:].split("|")[0])
            print(f"[focus={focus}]")
        elif line.startswith("CASE|"):
            _, name, text, shots = line.split("|", 3)
            expect = next(e for n, k, e in CASES if n == name)
            ok = (text == expect)
            detail = f"text={text!r} expect={expect!r}"
            if name == "csssss_stages":
                # 按 SHOT 序号判定：仅第 0 次 SHOT（'c'，有候选）应有可见候选窗；
                # 第 1/2 次（'cs'/'csssss'，无候选）窗口必须已隐藏
                import re as _re
                seqs = set()
                for x in shots.split(","):
                    if not x: continue
                    m = _re.search(r"_(\d+)_\d+_(?:pw|bb)\.bmp$", x.rsplit(":", 1)[0])
                    if m: seqs.add(int(m.group(1)))
                detail += f" | shots with visible window: {sorted(seqs)}"
                if seqs != {0}:
                    ok = False
                    detail += " FAIL(candidate window must hide when candidates become empty)"
                else:
                    detail += " OK(window hides when no candidates)"
            elif name == "theme_multi" and shots:
                # 关键断言：多候选窗的【非选中行】背景必须是主题深色
                # （历史 bug：窗口背景填充从未生效，未选中项显示为白——单项用例掩盖了它）
                f, dim = shots.split(",")[0].rsplit(":", 1)
                w, h, px = read_bmp(f)
                ys = range(int(h*0.55), max(int(h*0.55)+1, h-8))
                inner = [p for y in ys for p in px[y][8:max(9, w-8)]]
                dark = sum(1 for (r, g, b) in inner if (r+g+b)/3 < 90) / max(1, len(inner))
                white = sum(1 for (r, g, b) in inner if r > 200 and g > 200 and b > 200) / max(1, len(inner))
                detail += f" | shot {w}x{h} lowerRows dark%={dark:.0%} white%={white:.0%}"
                if dark < 0.5:
                    ok = False
                    detail += " FAIL(window bg not themed)"
                else:
                    detail += " OK(bg themed)"
            elif name == "theme_dark" and shots:
                for s in shots.split(","):
                    f, dim = s.rsplit(":", 1)
                    w, h, wr, lum, dr = shot_stats(f)
                    detail += f" | shot {w}x{h} white%={wr:.0%} lum={lum:.0f} dark%={dr:.0%}"
                    if dr < 0.5 and lum > 90:
                        ok = False
                        detail += " FAIL(not dark)"
                    else:
                        detail += " OK(dark)"
            elif name == "theme_dark" and not shots:
                ok = False
                detail += " | NO SHOT"
            print(("PASS " if ok else "FAIL ") + name + "  " + detail)
            results.append(ok)
    print(f"\nRESULT: {sum(results)}/{len(results)} passed  (dir {WORK})")
    sys.exit(0 if all(results) else 1)
