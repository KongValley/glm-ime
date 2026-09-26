#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 CC-CEDICT 构建 glm-ime 词库（双引擎同源）。
数据源：https://www.mdbg.net/chinese/export/cedict/cedict_1_0_ts_utf-8_mdbg.txt.gz
许可：CC-BY-SA 4.0（产物头写入来源与许可声明）。
用法：python tools/build_lexicon.py [cedict.txt.gz 路径]
     （缺省读 %TEMP%/cedict.txt.gz；不存在则经代理下载）"""
import gzip, json, os, sys, urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CEDICT_URL = "https://www.mdbg.net/chinese/export/cedict/cedict_1_0_ts_utf-8_mdbg.txt.gz"
PROXY = "http://127.0.0.1:7890"
MAX_FLAT_LEN = 8
TOP_PER_FLAT = 9

BUILTIN = [
    ("nihao", "你好", 20000), ("ni", "你", 19000), ("hao", "好", 19000),
    ("de", "的", 21000), ("shi", "是", 20000), ("wo", "我", 20000),
    ("ta", "他", 15000), ("ta", "她", 15000), ("men", "们", 14000),
    ("zhongwen", "中文", 15000), ("shurufa", "输入法", 15000),
    ("gongzuo", "工作", 15000), ("zhongguo", "中国", 16000),
    ("xihuan", "喜欢", 14000), ("pengyou", "朋友", 14000),
    ("jintian", "今天", 15000), ("mingtian", "明天", 14000),
    ("xiexie", "谢谢", 14000), ("ceshi", "测试", 12000),
    ("kaifa", "开发", 14000), ("ziyou", "自由", 12000),
    ("shijie", "世界", 15000), ("shijian", "时间", 14000),
    ("daxue", "大学", 13000), ("ai", "爱", 13000),
]

def tone_off(pinyin: str) -> str:
    """去声调数字 + u:→v + 小写。"""
    return "".join(c for c in pinyin if not c.isdigit()).lower().replace("u:", "v").strip()

def ensure_source(path: Path) -> Path:
    if path.exists():
        return path
    print("downloading CC-CEDICT via proxy ...")
    handler = urllib.request.ProxyHandler({"http": PROXY, "https": PROXY})
    req = urllib.request.Request(CEDICT_URL, headers={"User-Agent": "glm-ime/0.2"})
    with urllib.request.build_opener(handler).open(req, timeout=180) as resp:
        path.write_bytes(resp.read())
    return path

def main():
    gz = ensure_source(Path(sys.argv[1]) if len(sys.argv) > 1
                       else Path(os.environ.get("TEMP", "/tmp")) / "cedict.txt.gz")
    text = gzip.open(gz, "rt", encoding="utf-8").read()

    by_flat = {}      # flat -> {text: freq}
    syllables = set() # 实际出现的音节（DP 切分自洽）
    n = 0
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        parts = line.split(" ", 2)
        if len(parts) < 3 or not parts[2].startswith("["):
            continue
        simp = parts[1]
        pinyin_raw = parts[2][1:parts[2].index("]")] if "]" in parts[2] else ""
        if not simp or not pinyin_raw:
            continue
        py = tone_off(pinyin_raw)
        syls = py.split()
        if not syls:
            continue
        for syl in syls:
            if syl.isascii() and syl.isalnum():
                syllables.add(syl)
        flat = py.replace(" ", "")
        if not flat or len(flat) > MAX_FLAT_LEN or not flat.isascii():
            continue
        # 过滤非汉字词条（CC-CEDICT 含大量字母/数字词条，如 "CP"）
        if not simp or simp.isascii():
            continue
        # 过滤含生僻字的词条：只保留 GB2312 可编码汉字（6763 常用/次常用字）+ ASCII
        def common_han(ch):
            if ch.isascii():
                return ch.isalnum()
            try:
                ch.encode("gb2312")
                return True
            except UnicodeEncodeError:
                return False
        if not all(common_han(ch) for ch in simp):
            continue
        freq = 10000 - len(flat) * 500   # 启发式：短词优先
        slot = by_flat.setdefault(flat, {})
        slot[simp] = max(slot.get(simp, 0), freq)
        n += 1

    for flat, t, f in BUILTIN:
        slot = by_flat.setdefault(flat, {})
        slot[t] = max(slot.get(t, 0), f)

    # 首字母缩写：syllables 贪心最长匹配切分后取各音节首字母（支持简拼输入）
    syl_by_len = sorted(syllables, key=len, reverse=True)
    def abbr_of(flat):
        out = []
        i = 0
        while i < len(flat):
            for syl in syl_by_len:
                if flat.startswith(syl, i):
                    out.append(syl[0])
                    i += len(syl)
                    break
            else:
                out.append(flat[i])   # 无法切分（非拼音串）：原样取字符
                i += 1
        return "".join(out)

    words = []
    for flat, texts in by_flat.items():
        ab = abbr_of(flat)
        for t, f in sorted(texts.items(), key=lambda x: (-x[1], x[0]))[:TOP_PER_FLAT]:
            words.append({"py": flat, "ab": ab, "text": t, "freq": f})
    words.sort(key=lambda w: (w["py"], -w["freq"], w["text"]))

    out = {
        "_source": "CC-CEDICT (https://www.mdbg.net/chinese/dictpage.aspx) CC-BY-SA 4.0; built by tools/build_lexicon.py",
        "syllables": sorted(syllables),
        "words": words,
    }
    payload = json.dumps(out, ensure_ascii=False, separators=(",", ":"))
    for dest in (ROOT / "engine-py" / "lexicon.json", ROOT / "crates" / "engine-rs" / "lexicon.json"):
        dest.write_text(payload, encoding="utf-8")
    print(f"parsed: {n}  entries: {len(words)}  syllables: {len(syllables)}  size: {len(payload)//1024} KB")
    print("written: engine-py/lexicon.json, crates/engine-rs/lexicon.json")

if __name__ == "__main__":
    main()
