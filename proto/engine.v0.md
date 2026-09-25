# 引擎协议 engine.v0

两引擎（engine-py / engine-rs）的唯一契约。传输：stdio，每请求一行 UTF-8 JSON，每响应一行 UTF-8 JSON。诊断信息只准走 stderr。

## 请求

```json
{"op": "init",  "user_dir": "C:/Users/x/glm-ime"}
{"op": "key",   "code": 65, "char": "a", "mods": 0}
{"op": "reset"}
{"op": "shutdown"}
```

- `code`：Windows VK 码。`char`：可打印字符（小写化后的字母），无则空串。
- 引擎持有全部会话状态（缓冲、候选），shell 无状态。

## 响应

```json
{"ok": true, "consumed": true, "actions": [...]}
```

- `consumed=false` → shell 把按键穿透给应用（动作仍可能为空）。
- `actions`（有序）：

| action | 字段 | 语义 |
|---|---|---|
| composition | text, cursor | 更新组词串（空串=结束组词）|
| candidates | list | 替换候选列表（空=关窗）|
| commit | text | 上屏文本 |
| message | text, seconds | 浮动提示（模式切换等）|

## 按键语义（v0 固定，两引擎必须逐字节一致）

| 输入 | 行为 |
|---|---|
| 字母 a-z | 追加到缓冲，重算候选；consumed=true |
| 空格 | 有候选→commit 第 1 个；无候选但有缓冲→commit 原始缓冲；然后清空 |
| 数字 1-9 | 有候选→选第 N 个（越界则穿透）；无候选→穿透 |
| Backspace | 有缓冲→删尾并重算；空→穿透 |
| Enter | 有缓冲→commit 原始缓冲；空→穿透 |
| Esc | 有缓冲/候选→清空；空→穿透 |
| 其他 | 穿透 |

## 查询规则（v0）

词库：`lexicon.json`，`words: [{py, text, freq}]`，`py` 为空格分隔音节。
匹配（对缓冲原始字母 `raw`，及各词 `flat = py 去空格`）：

- 精确：`flat == raw` → 排序分 `freq + 10000`
- 前缀：`flat.startswith(raw)` → 排序分 `freq`
- 按分数降序去重，取前 9。

`init` 时若 `user_dir/lexicon.json` 存在则覆盖内置词库；用户词自学习属 M3。

## 生命周期

- 引擎被 launcher spawn 后阻塞读 stdin；`shutdown` 或 EOF 退出，exit 0。
- 响应必须含 `ok`；引擎内部异常 → `{"ok": false, "error": "..."}`，进程不崩。


---

# M3 扩展（已实现，协议容差声明）

## Tab = 整句生成
- 有缓冲时 Tab 触发整句：引擎若支持 → 吃键并返回 candidates（整句候选）；
  不支持 → 穿透（consumed=false）。**两引擎行为允许分歧**（容差）。
- engine-py：LLM 优先（`user_dir/config.json` 的 `llm` 节，OpenAI 兼容 API，
  无 key/disabled/超时/失败 → 本地 DP 音节切分 + 词典贪心组合兜底）。
- engine-rs：v0.2 不支持整句，穿透。

## 自学习（双引擎对称）
- 每次 commit 非 ASCII 文本：以当前缓冲码为 key 写 `user_dir/user_lexicon.json`
  （`{words: {code: [[text, freq], ...]}}`，freq 递增，每码保留 Top9）。
- init 时加载合并进词库（freq 10000 高权重，优先于内置词库）。
- user_dir 来源：TIP init 请求传 `%LOCALAPPDATA%\glm-ime`。

## config.json（user_dir 下，可选）
```json
{"llm": {"enabled": true, "base_url": "https://api.openai.com/v1",
         "api_key": "sk-...", "model": "gpt-4o-mini", "timeout_sec": 3}}
```
