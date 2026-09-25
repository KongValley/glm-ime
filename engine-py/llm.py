"""glm-ime 引擎 LLM 客户端（M3）。
OpenAI 兼容 /v1/chat/completions；stdlib urllib，零依赖。
配置来自 user_dir/config.json 的 "llm" 节：
  {"llm": {"enabled": true, "base_url": "https://.../v1",
           "api_key": "sk-...", "model": "...", "timeout_sec": 3}}
无配置/disabled/任何异常 → 调用方走本地兜底。"""
import json
import urllib.request
from pathlib import Path

DEFAULT_CONFIG = {
    "enabled": False,
    "base_url": "https://api.openai.com/v1",
    "api_key": "",
    "model": "gpt-4o-mini",
    "timeout_sec": 3,
}


def load_llm_config(user_dir):
    cfg = dict(DEFAULT_CONFIG)
    try:
        p = Path(user_dir) / "config.json" if user_dir else None
        if p and p.exists():
            data = json.loads(p.read_text(encoding="utf-8"))
            cfg.update(data.get("llm", {}) or {})
    except Exception:
        pass
    return cfg


def llm_available(cfg):
    return bool(cfg.get("enabled") and cfg.get("api_key") and cfg.get("base_url") and cfg.get("model"))


def chat(cfg, system_prompt, user_prompt):
    """同步调用 chat/completions，返回首个 choice 文本；失败抛异常（调用方兜底）。"""
    url = cfg["base_url"].rstrip("/") + "/chat/completions"
    body = json.dumps({
        "model": cfg["model"],
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_prompt},
        ],
        "temperature": 0.3,
        "max_tokens": 200,
    }).encode("utf-8")
    req = urllib.request.Request(url, data=body, method="POST", headers={
        "Content-Type": "application/json",
        "Authorization": f"Bearer {cfg['api_key']}",
    })
    timeout = float(cfg.get("timeout_sec", 3))
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    return data["choices"][0]["message"]["content"]


def candidates_from_reply(text):
    """把模型回复解析为候选词列表（容忍各种输出形态）。"""
    if not text:
        return []
    # 优先尝试 JSON 数组
    s = text.strip()
    start = s.find("[")
    end = s.rfind("]")
    if start != -1 and end > start:
        try:
            arr = json.loads(s[start:end + 1])
            out = []
            for item in arr:
                if isinstance(item, str) and item.strip():
                    out.append(item.strip())
                elif isinstance(item, dict):
                    t = item.get("text") or item.get("sentence") or ""
                    if t.strip():
                        out.append(t.strip())
            return out[:9]
        except Exception:
            pass
    # 退路：按行取非空行
    lines = [l.strip(" \t0123456789.、)（(：:-") for l in s.splitlines()]
    return [l for l in lines if l][:9]
