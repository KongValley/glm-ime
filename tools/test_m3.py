#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""M3 测试：用户词典自学习（双引擎对称）+ Tab 整句（py LLM/DP 兜底，rs 穿透）。
用法：python tools/test_m3.py"""
import json, subprocess, sys, tempfile, os, time, shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PY_ENGINE = [sys.executable, str(ROOT / "engine-py" / "glm_engine.py")]
RS_EXE = ROOT / "crates" / "engine-rs" / "target" / "release" / "glm-engine-rs.exe"

def spawn(cmd):
    return subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, encoding="utf-8", bufsize=1)

def send(p, msg):
    p.stdin.write(json.dumps(msg, ensure_ascii=False) + "\n")
    p.stdin.flush()
    while True:
        line = p.stdout.readline()
        if not line:
            raise RuntimeError("engine died")
        if line.startswith("{"):
            return json.loads(line)

def key(p, code, ch=""):
    return send(p, {"op": "key", "code": code, "char": ch, "mods": 0})

def commits_of(reply):
    return [a["text"] for a in reply.get("actions", []) if a["action"] == "commit"]

def test_userdict(cmd, name):
    """自学习：打 nihao+空格 提交"你好" → user_lexicon 落盘 → 重启后 hot-load。"""
    tmp = tempfile.mkdtemp(prefix="glm_m3_")
    ok = True
    try:
        p = spawn(cmd)
        send(p, {"op": "init", "user_dir": tmp})
        for ch in "nihao":
            key(p, ord(ch), ch)
        r = key(p, 32)
        assert commits_of(r) == ["你好"], r
        send(p, {"op": "shutdown"}); p.wait(timeout=10)

        # 落盘验证
        ud = json.loads((Path(tmp) / "user_lexicon.json").read_text(encoding="utf-8"))
        assert "nihao" in ud["words"], ud
        assert any(e[0] == "你好" for e in ud["words"]["nihao"]), ud

        # 重启加载 + 二次提交后频次提升
        p = spawn(cmd)
        r = send(p, {"op": "init", "user_dir": tmp})
        for ch in "nihao":
            key(p, ord(ch), ch)
        key(p, 32)
        send(p, {"op": "shutdown"}); p.wait(timeout=10)
        ud2 = json.loads((Path(tmp) / "user_lexicon.json").read_text(encoding="utf-8"))
        freq = next(e[1] for e in ud2["words"]["nihao"] if e[0] == "你好")
        assert freq >= 2, ud2
        print(f"  [{name}] user dict learn+reload: PASS (freq={freq})")
    except Exception as e:
        print(f"  [{name}] user dict: FAIL {e}")
        ok = False
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return ok

def test_py_tab_fallback():
    """py 引擎：无 LLM 配置 → Tab 用本地 DP 兜底产出整句候选。"""
    tmp = tempfile.mkdtemp(prefix="glm_m3_py_")
    ok = True
    try:
        p = spawn(PY_ENGINE)
        r = send(p, {"op": "init", "user_dir": tmp})
        assert r["info"]["llm"] is False, r  # 无 config → LLM 不可用
        for ch in "nihao":
            key(p, ord(ch), ch)
        r = key(p, 9)  # Tab
        cands = [a["list"] for a in r.get("actions", []) if a["action"] == "candidates"]
        assert cands, f"Tab no candidates: {r}"
        # 本地 DP：nihao 应组合出含"你好"的句子或单字组合
        top = cands[-1][0]
        assert any(c in top for c in ("你", "好")), f"unexpected composition: {cands}"
        print(f"  [engine-py] Tab local-DP fallback: PASS (top={top!r})")
        send(p, {"op": "shutdown"}); p.wait(timeout=10)
    except Exception as e:
        print(f"  [engine-py] Tab fallback: FAIL {e}")
        ok = False
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return ok

def test_rs_tab_passthrough():
    """rs 引擎：Tab 穿透（协议容差声明）。"""
    p = spawn(RS_EXE)
    send(p, {"op": "init", "user_dir": ""})
    r = key(p, 9)
    passthrough = (r.get("consumed") is False)
    send(p, {"op": "shutdown"}); p.wait(timeout=10)
    print(f"  [engine-rs] Tab passthrough: {'PASS' if passthrough else 'FAIL'}")
    return passthrough

if __name__ == "__main__":
    if not RS_EXE.exists():
        subprocess.run(["cargo", "build", "--release"], cwd=ROOT / "crates" / "engine-rs", check=True)
    print("=== M3 tests ===")
    results = [
        test_userdict(PY_ENGINE, "engine-py"),
        test_userdict([str(RS_EXE)], "engine-rs"),
        test_py_tab_fallback(),
        test_rs_tab_passthrough(),
    ]
    print("M3:", "ALL PASSED" if all(results) else "FAILURES")
    sys.exit(0 if all(results) else 1)
