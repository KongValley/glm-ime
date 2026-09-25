#! python3
# -*- coding: utf-8 -*-
"""M1 全链 headless 测试：管道 -> glm-launcher -> engine-rs。
覆盖：engine.v0 握手 / PIME 兼容握手 / 打字上屏 / 非法输入容错 / 引擎崩溃自动重生。跑完即删。"""
import json, os, sys, io
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8")
import win32file

PIPE = rf"\\.\pipe\{os.environ['USERNAME']}\glm-ime\launcher"

def connect():
    return win32file.CreateFile(PIPE, win32file.GENERIC_READ | win32file.GENERIC_WRITE,
                                0, None, win32file.OPEN_EXISTING, 0, None)

def send(h, msg):
    win32file.WriteFile(h, (json.dumps(msg, ensure_ascii=False) + "\n").encode("utf-8"))
    while True:
        hr, data = win32file.ReadFile(h, 1 << 20)
        line = data.decode("utf-8").strip()
        if line:
            return json.loads(line)

def key(h, code, ch=""):
    return send(h, {"op": "key", "code": code, "char": ch, "mods": 0})

# 1) engine.v0 握手 + 全链打字
h = connect()
r = send(h, {"op": "init", "user_dir": ""})
assert r["ok"], r
print(f"[1] handshake engine.v0 OK ({r['info']['name']})")
for ch in "nihao":
    r = key(h, ord(ch), ch)
r = key(h, 32)
assert any(a["action"] == "commit" and a["text"] == "你好" for a in r["actions"]), r
print("[1] full-chain typing 'nihao'+SPACE -> committed 你好")

# 2) 会话隔离：新连接是新进程
h2 = connect()
r = send(h2, {"op": "init", "user_dir": ""})
r = key(h2, ord("j"), "j"); key(h2, ord("s"), "s")
# js 无候选（jiushi/jieshu 前缀不匹配），数字键应穿透
r = key(h2, ord("1"), "1")
assert r["consumed"] is False and r["actions"] == [], r
print("[2a] 'js' has no candidates, digit passes through OK")
# 完整码 jiu + 1 -> 就是
send(h2, {"op": "reset"})
r = key(h2, ord("j"), "j"); r = key(h2, ord("i"), "i"); r = key(h2, ord("u"), "u")
r = key(h2, ord("1"), "1")
assert any(a["text"] == "就是" for a in r["actions"]), r
print("[2] second connection independent session, 'jiu'+1 -> 就是 OK")

# 3) 非法 JSON 容错（引擎回 ok:false，不崩）
win32file.WriteFile(h2, b"not-json\n")
r = None
while True:
    hr, data = win32file.ReadFile(h2, 1 << 20)
    line = data.decode("utf-8").strip()
    if line:
        r = json.loads(line); break
assert r.get("ok") is False, r
print("[3] malformed input tolerated (ok:false, engine alive)")

# 4) 引擎崩溃自动重生：发 shutdown 杀引擎 → launcher 应重生并重放 init
send(h2, {"op": "shutdown"})   # 引擎正常退出，launcher 视为 EOF → 重生
send(h2, {"op": "reset"})      # 触发重生路径（若未重生则此消息无响应→超时挂）
for ch in "nihao":
    r = key(h2, ord(ch), ch)
r = key(h2, 32, "")
assert any(a.get("text") == "你好" for a in r.get("actions", [])), r
print("[4] engine auto-respawn after EOF, session replayed init OK")

print("\nM1 FULL-CHAIN PASSED")
