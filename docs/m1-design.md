# M1 设计决策记录：launcher 服务层

日期：2026-09-25 · 代码：`crates/launcher`（tokio 版，~200 行）

## 关键决策

### D1 进程即会话（每连接一引擎进程）
多会话隔离不靠引擎内 client_id 多路复用，而是每条命名管道连接 spawn 一个独立引擎进程。
- engine-rs 冷启动 4.4ms（M0 实测），进程隔离 = 会话隔离 = 崩溃隔离，三合一。
- 代价：engine-py 冷启动 48.8ms，多应用切换可感知 → 默认引擎 engine-rs。

### D2 双向行中继用 tokio::select，不用共享锁
引擎 stdout/stdin 与管道读写必须同时等待。曾试同步线程 + `parking_lot::Mutex` 保护
引擎槽：`read_line` 持锁阻塞 → 发送线程饿死 → 死锁。select 双向 + 所有权分离
（stdout 归转发线程侧、stdin 归接收侧）无锁无死锁。

### D3 响应行数守恒（本次调试 3 轮得出的核心不变量）
**客户端每发一行，恰收一行。** 偏移源：重生后引擎对重放 init 的回复是"多出来的一行"。
- 初次握手：launcher 转发 init 给引擎，引擎回复经 select 正常转发客户端（**不吃**）。
- 重生路径：init 重放后，launcher **必须吃掉** init 回复，否则客户端后续每条请求
  收到的都是上一条的响应（错位一拍）。
- 修复前症状：shutdown 后 reset 挂起 / init 无响应——都是行数守恒被破坏。

### D4 崩溃重生语义
引擎进程退出（崩溃或 shutdown 自杀）→ launcher 检测 stdout EOF → kill 兜底 →
spawn 新引擎 → 重放 init → 吃掉 init 回复 → 会话状态归零继续。
上限 5 次连续重生防 spawn 炸弹。客户端无感知断线。

### D5 部署布局
```
安装目录/
  glm-launcher.exe          # 常驻单例（Global mutex 防双开）
  glm-engine-rs.exe         # 默认引擎（exe 同目录自动发现）
  engine.json               # 可选：GLM_ENGINE_CMD 覆盖引擎命令行
```
环境变量 `GLM_ENGINE_CMD="python engine-py/glm_engine.py"` 可热切双引擎（测试用）。

## 验证（tools/test_m1.py，全过）

| 用例 | 结论 |
|---|---|
| engine.v0 握手 + nihao+space → 你好 | 管道→launcher→引擎全链 OK |
| 双连接独立会话（jiu+1 → 就是） | 进程即会话隔离 OK |
| 非法 JSON → ok:false，引擎不崩 | 容错 OK |
| shutdown 自杀 → 自动重生 → 重放 init → 继续打字 | 崩溃重生 OK |
