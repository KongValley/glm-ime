#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""glm-ime 双引擎对比 harness（M0 验收）。
1) golden 用例：键序列 → 期望动作，两引擎结果必须一致且符合预期
2) 跑分：冷启动 / 单键 RTT P50/P99 / 内存
用法：python tools/compare.py   （会自动 cargo build --release engine-rs）"""
import json, subprocess, sys, time, os, statistics, io
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8")

PY_ENGINE = [sys.executable, str(ROOT / "engine-py" / "glm_engine.py")]
RS_EXE = ROOT / "crates" / "engine-rs" / "target" / "release" / "glm-engine-rs.exe"
CASES = json.loads((ROOT / "tools" / "golden" / "cases.json").read_text(encoding="utf-8"))

def spawn(cmd):
    return subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, encoding="utf-8", bufsize=1)

def send(p, msg):
    p.stdin.write(json.dumps(msg, ensure_ascii=False) + "\n")
    p.stdin.flush()
    line = p.stdout.readline()
    if not line:
        err = p.stderr.read() if p.poll() is not None else ""
        raise RuntimeError(f"engine died: {err[:200]}")
    return json.loads(line)

def mem_of(pid):
    try:
        out = subprocess.check_output(
            ["powershell", "-NoProfile", "-Command",
             f"(Get-Process -Id {pid}).WorkingSet64"]).decode().strip()
        return int(out) / 1024 / 1024
    except Exception:
        return float("nan")

def run_case(p, case):
    """回放用例，返回 (commit 历史, 最后一次 reply)。"""
    send(p, {"op": "reset"})
    commits, last = [], None
    for k in case.get("keys_real", []):
        code, ch = (int(k[0]), k[1]) if isinstance(k[0], int) else (ord(k[0]), k[1])
        r = send(p, {"op": "key", "code": code, "char": ch, "mods": 0})
        last = r
        for a in r.get("actions", []):
            if a["action"] == "commit":
                commits.append(a["text"])
    return commits, last

def last_actions(reply, action):
    return [a for a in (reply or {}).get("actions", []) if a["action"] == action]

def check_engine(name, cmd):
    p = spawn(cmd)
    t0 = time.perf_counter()
    r = send(p, {"op": "init", "user_dir": ""})
    cold = time.perf_counter() - t0
    assert r.get("ok"), r

    results, fails = [], []
    for case in CASES["cases"]:
        commits, last = run_case(p, case)
        ok = True
        if "expect_commit" in case:
            ok = bool(commits) and commits[-1] == case["expect_commit"]
        if ok and "expect_consumed" in case:
            ok = last is not None and last.get("consumed") == case["expect_consumed"]
        if ok and "expect_last_candidates_contain" in case:
            c = last_actions(last, "candidates")
            ok = bool(c) and case["expect_last_candidates_contain"] in c[-1]["list"]
        if ok and "expect_last_candidates" in case:
            c = last_actions(last, "candidates")
            ok = bool(c) and c[-1]["list"] == case["expect_last_candidates"]
        if ok and "expect_composition_cleared" in case:
            comps = last_actions(last, "composition")
            ok = bool(comps) and comps[-1]["text"] == ""
        results.append((case["name"], ok))
        if not ok:
            fails.append((case["name"], commits, last))

    # RTT：reset 后单键 'n' 往返（缓冲增长，模拟真实最坏路径之一）
    code, ch = CASES["bench"]["rtt_key"]
    rtts = []
    for i in range(CASES["bench"]["rtt_iterations"]):
        t = time.perf_counter()
        send(p, {"op": "key", "code": ord(code), "char": ch, "mods": 0})
        rtts.append(time.perf_counter() - t)
        if (i + 1) % 20 == 0:  # 定期 reset 防止缓冲无限增长偏离真实分布
            send(p, {"op": "reset"})
    mem = mem_of(p.pid)
    send(p, {"op": "shutdown"})
    try:
        p.stdin.close()
        p.wait(timeout=10)
    except Exception:
        p.kill()
    return {"name": name, "cold": cold, "fails": fails, "results": results,
            "rtt": rtts, "mem": mem}

def main():
    if not RS_EXE.exists() or (ROOT / "crates" / "engine-rs" / "src" / "main.rs").stat().st_mtime > RS_EXE.stat().st_mtime:
        print("building engine-rs ...")
        subprocess.run(["cargo", "build", "--release"],
                       cwd=ROOT / "crates" / "engine-rs", check=True)

    report = []
    for name, cmd in [("engine-py", PY_ENGINE), ("engine-rs", [str(RS_EXE)])]:
        rep = check_engine(name, cmd)
        rtts = rep["rtt"]
        rep["p50"] = statistics.median(rtts) * 1000
        rep["p99"] = sorted(rtts)[max(0, int(len(rtts) * 0.99) - 1)] * 1000
        report.append(rep)

    print("\n=== glm-ime engine comparison (M0) ===")
    for rep in report:
        passed = sum(1 for _, ok in rep["results"] if ok)
        print(f"\n[{rep['name']}]")
        print(f"  golden: {passed}/{len(rep['results'])} passed")
        for cname, commits, last in rep["fails"]:
            preview = json.dumps(last, ensure_ascii=False)[:160] if last else "None"
            print(f"    FAIL {cname}: commits={commits} last={preview}")
        print(f"  cold start : {rep['cold']*1000:8.1f} ms")
        print(f"  key RTT    : P50 {rep['p50']:.2f} ms   P99 {rep['p99']:.2f} ms")
        print(f"  memory RSS : {rep['mem']:.1f} MB")

    if all(not r["fails"] for r in report):
        print("\nVERDICT: both engines pass all golden cases")
    else:
        sys.exit(1)

if __name__ == "__main__":
    main()
