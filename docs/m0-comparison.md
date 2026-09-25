# M0 对比报告：engine-py vs engine-rs

日期：2026-09-25 · harness：`tools/compare.py` · 词库 28 词 · golden 10 用例

## 结果

| 指标 | engine-py | engine-rs |
|---|---|---|
| golden 正确性 | **10/10** | **10/10** |
| 冷启动（spawn+init） | 48.8 ms | **4.4 ms** |
| 单键 RTT P50 | 0.04 ms | **0.03 ms** |
| 单键 RTT P99 | 0.09 ms | **0.06 ms** |
| 内存 RSS | 11.7 MB | **3.7 MB** |
| 产物 | 需 Python 运行时(~100MB 打包) | **单 exe ~500KB** |

RTT 两者同为亚 0.1ms：瓶颈在 stdio 管道与 JSON 序列化，语言差异被 I/O 淹没。

## 结论

1. **行为一致性达成**：同协议两实现 10/10 一致，协议 engine.v0 有效。
2. **性能差距在冷启动与内存**（11× / 3×），对"每会话一进程"的 launcher 模型，rs 冷启动 4.4ms 使进程池几乎免费；py 49ms 在多应用切换时可感知。
3. **决策**：M1 launcher 默认引擎 = engine-rs；engine-py 保留作为 M3 LLM 层的实现载体（对比实验继续）。
