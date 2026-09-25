#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""glm-ime 引擎 — Python 实现（协议 engine.v0 + M3 扩展：Tab 整句 / 用户词典 / LLM）。
stdio JSON Lines；诊断只走 stderr。

M3 扩展语义（相对 engine.v0）：
  Tab        触发整句生成：LLM 可用 → LLM 候选；否则本地 DP 拼音切分兜底。
             不支持整句的引擎（engine-rs）对 Tab 穿透 —— 协议容差声明。
  自学习     每次 commit 非 ASCII 文本时，把 (当前缓冲码 → 提交文本) 记入
             user_lexicon.json（freq 递增），下次启动合并加载。"""
import sys, json, os
from pathlib import Path

_HERE = Path(__file__).parent
WORDS = []          # [(py_flat, text, freq)]，含用户词典
SYLLABLES = set()   # 音节表（整句切分用）
USER_DICT_PATH = None
LLM_CFG = {}


def load_lexicon(user_dir):
    """内置词库 + 用户词典合并；音节表载入。"""
    builtin = _HERE / "lexicon.json"
    src = builtin
    if user_dir:
        p = Path(user_dir) / "lexicon.json"
        if p.exists():
            src = p
    data = json.loads(src.read_text(encoding="utf-8"))
    words = [(w["py"], w["text"], int(w["freq"])) for w in data["words"]]
    syllables = set(data.get("syllables", []))
    # 用户词典（同目录）：flat 匹配词形 {flat: [[text, freq], ...]}
    if user_dir:
        up = Path(user_dir) / "user_lexicon.json"
        if up.exists():
            try:
                ud = json.loads(up.read_text(encoding="utf-8"))
                for flat, entries in ud.get("words", {}).items():
                    for text, freq in entries:
                        words.append((flat, text, int(freq)))
            except Exception as e:
                print(f"user dict load failed: {e}", file=sys.stderr)
    return words, syllables


def lookup(raw):
    """精确(freq+10000) > 前缀(freq)，降序去重取前 9。规则见 proto。"""
    score = {}
    for py, text, freq in WORDS:
        flat = py.replace(" ", "")
        if flat == raw:
            s = freq + 10000
        elif raw and flat.startswith(raw):
            s = freq
        else:
            continue
        if text not in score or score[text] < s:
            score[text] = s
    return sorted(score, key=lambda t: (-score[t], t))[:9]


# ---------------------------------------------------------------------------
# 整句：音节 DP 切分 + 词典词组合（LLM 兜底前的本地路径）
# ---------------------------------------------------------------------------

def dp_split(raw, syllables):
    """把拼音串按音节表动态规划切分，返回所有可行切分（最多 8 条），按长度稳定排序。"""
    n = len(raw)
    from functools import lru_cache
    paths = []

    def rec(i, acc):
        if len(paths) >= 8:
            return
        if i == n:
            paths.append(list(acc))
            return
        for j in range(min(n, i + 6), i, -1):
            if raw[i:j] in syllables:
                acc.append(raw[i:j])
                rec(j, acc)
                acc.pop()

    rec(0, [])
    return paths


def compose_sentence(raw):
    """本地整句组合：最优切分下贪心拼接词典词。返回候选句列表（可空）。"""
    if not SYLLABLES:
        return []
    best = None
    for path in dp_split(raw, SYLLABLES):
        if best is None or len(path) < len(best):
            best = path
    if not best:
        return []
    # 在最优切分的音节序列上做词典贪心组合，产出句子
    out = []
    combos = []

    def combine(i, acc):
        if len(combos) >= 5:
            return
        if i == len(best):
            combos.append("".join(acc))
            return
        # 先试最长词典词（覆盖 1..4 个音节）
        for span in (4, 3, 2, 1):
            if i + span > len(best):
                continue
            key = "".join(best[i:i + span])
            cands = lookup(key)
            exact = [c for c in cands]
            if exact:
                for c in exact[:2]:
                    acc.append(c)
                    combine(i + span, acc)
                    acc.pop()
                return  # 该位置用了词典词即前进（贪心）
        # 无词典覆盖：音节直拼（用 lookup 兜底单字）
        key = best[i]
        cands = lookup(key)
        acc.append(cands[0] if cands else key)
        combine(i + 1, acc)
        acc.pop()

    combine(0, [])
    return combos


def llm_sentence(raw):
    """LLM 整句：拼音串 → 候选句列表。不可用/失败返回 []。"""
    import llm
    if not llm.llm_available(LLM_CFG):
        return []
    sys_p = "你是拼音输入法的整句转换引擎。用户给你一串无空格全拼，输出最可能的中文句子。"
    usr_p = (f"拼音: {raw}\n"
             f"输出 JSON 字符串数组，最多 5 个候选，按可能性排序，只输出 JSON。")
    try:
        text = llm.chat(LLM_CFG, sys_p, usr_p)
        return llm.candidates_from_reply(text)
    except Exception as e:
        print(f"llm failed: {e}", file=sys.stderr)
        return []


class Session:
    """会话状态机。step() 返回 (consumed, actions)；基础语义锁定 proto/engine.v0.md。"""

    def __init__(self):
        self.raw = ""
        self.cands = []

    def _acts(self):
        acts = []
        if self.raw:
            acts.append({"action": "composition", "text": self.raw, "cursor": len(self.raw)})
        if self.cands:
            acts.append({"action": "candidates", "list": self.cands})
        return acts

    def _clear(self):
        self.raw = ""
        self.cands = []
        return [{"action": "composition", "text": "", "cursor": 0},
                {"action": "candidates", "list": []}]

    def step(self, code, char, mods):
        if "a" <= char <= "z":
            self.raw += char
            self.cands = lookup(self.raw)
            return True, self._acts()
        if code == 9:  # Tab：整句生成（M3）
            if not self.raw:
                return False, []
            sent = llm_sentence(self.raw) or compose_sentence(self.raw)
            if sent:
                self.cands = sent[:9]
                return True, [{"action": "candidates", "list": self.cands}] + self._acts()
            return True, self._acts()
        if code == 32:  # space
            if not self.raw:
                return False, []
            text = self.cands[0] if self.cands else self.raw
            learn(self.raw, text)
            return True, [{"action": "commit", "text": text}] + self._clear()
        if ord("1") <= code <= ord("9"):
            idx = code - ord("1")
            if self.cands and idx < len(self.cands):
                text = self.cands[idx]
                learn(self.raw, text)
                return True, [{"action": "commit", "text": text}] + self._clear()
            return False, []
        if code == 8:  # backspace
            if not self.raw:
                return False, []
            self.raw = self.raw[:-1]
            self.cands = lookup(self.raw) if self.raw else []
            return True, self._acts()
        if code == 13:  # enter
            if not self.raw:
                return False, []
            learn(self.raw, self.raw) if self.raw.isascii() else None
            return True, [{"action": "commit", "text": self.raw}] + self._clear()
        if code == 27:  # esc
            if not self.raw and not self.cands:
                return False, []
            return True, self._clear()
        return False, []  # 其他键穿透


def learn(code, text):
    """自学习：非 ASCII 提交文本 + 编码 → user_lexicon.json（freq 递增）。"""
    if not text or text.isascii() or not code:
        return
    try:
        ud = {}
        if USER_DICT_PATH and USER_DICT_PATH.exists():
            ud = json.loads(USER_DICT_PATH.read_text(encoding="utf-8"))
        words = ud.setdefault("words", {})
        entries = words.setdefault(code, [])
        for e in entries:
            if e[0] == text:
                e[1] = int(e[1]) + 1
                break
        else:
            entries.append([text, 1])
            if len(entries) > 9:
                entries.sort(key=lambda x: -int(x[1]))
                del entries[9:]
        if USER_DICT_PATH:
            USER_DICT_PATH.parent.mkdir(parents=True, exist_ok=True)
            USER_DICT_PATH.write_text(json.dumps(ud, ensure_ascii=False, indent=1), encoding="utf-8")
            # 热合并进当前词库（免重启）
            WORDS.append((code, text, 10000))
    except Exception as e:
        print(f"learn failed: {e}", file=sys.stderr)


SESSION = Session()


def handle(msg):
    op = msg.get("op")
    if op == "init":
        global WORDS, SYLLABLES, USER_DICT_PATH, LLM_CFG
        user_dir = msg.get("user_dir", "")
        WORDS, SYLLABLES = load_lexicon(user_dir)
        USER_DICT_PATH = Path(user_dir) / "user_lexicon.json" if user_dir else None
        import llm as _llm
        LLM_CFG = _llm.load_llm_config(user_dir)
        return {"ok": True, "info": {"name": "glm-ime-py", "version": "0.2",
                                     "llm": _llm.llm_available(LLM_CFG)}}
    if op == "reset":
        SESSION.__init__()
        return {"ok": True, "consumed": True, "actions": []}
    if op == "key":
        consumed, actions = SESSION.step(int(msg.get("code", 0)), str(msg.get("char", "")), int(msg.get("mods", 0)))
        return {"ok": True, "consumed": consumed, "actions": actions}
    if op == "shutdown":
        return None
    return {"ok": False, "error": f"unknown op {op!r}"}


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            reply = handle(json.loads(line))
            if reply is None:  # shutdown
                sys.stdout.write(json.dumps({"ok": True}, ensure_ascii=False) + "\n")
                sys.stdout.flush()
                return 0
        except Exception as e:  # 进程不崩：协议级错误回 ok:false
            print(f"engine error: {e!r}", file=sys.stderr)
            reply = {"ok": False, "error": str(e)}
        sys.stdout.write(json.dumps(reply, ensure_ascii=False) + "\n")
        sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
