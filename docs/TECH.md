# glm-ime 技术文档（TECH.md）

> **本文档是开发的唯一规范源**。所有修复与开发必须按本文档流程进行：
> 问题先登记台账 → 评估影响面 → 实现 → 自动化验证 → 部署 → 更新台账。
> **违反流程的即兴调试（尤其是直接操作用户桌面/前台的测试）一律禁止。**

版本：v0.3-draft　更新：2026-09-26　最后提交：`7ef36f1`

---

## 1. 系统架构

```
┌──────────────────────────────────────────────────────┐
│ glm-ime-tip.dll (C++17 TSF TIP, x86/x64, 静态CRT /MT) │
│  薄壳：按键→管道转发；动作→composition/候选窗/上屏      │
│  防护：SEH 全入口包裹（宿主进程绝不因 TIP 崩溃）         │
└───────────────┬──────────────────────────────────────┘
                │ named pipe  \\.\pipe\<user>\glm-ime\launcher
┌───────────────▼──────────────────────────────────────┐
│ glm-launcher.exe (Rust/tokio, 单例常驻)               │
│  管道↔引擎进程桥；进程即会话；请求队列+超时自愈          │
└───────────────┬──────────────────────────────────────┘
                │ stdio JSON Lines（协议 engine.v0）
      ┌─────────┴──────────┐
      ▼                    ▼
 glm-engine-rs.exe     engine-py/glm_engine.py
 (默认引擎, 3.6ms冷启)  (智能引擎: LLM整句+DP兜底; 可选)
```

### 关键文件职责

| 路径 | 职责 |
|---|---|
| `tip/src/GlmTextService.cpp` | TIP 主类：按键/动作映射；session 化 Start/EndComposition（防嵌套同步会话）|
| `tip/src/PipeClient.cpp` | 管道客户端：OVERLAPPED+超时、退避、launcher 自愈拉起 |
| `tip/src/dllmain.cpp` | COM 注册；DllMain 引导日志 |
| `tip/libIME2/` | vendored TSF 包装库（LGPL；改动见台账 B1/B9）|
| `crates/launcher/src/main.rs` | 管道服务：会话/请求队列/引擎重生重放/llog 日志 |
| `crates/engine-rs/` | 默认引擎（本地词库/自学习）|
| `engine-py/` | Python 引擎（LLM 整句/DP 切分；自学习；LLM 配置 `config.json`）|
| `tools/auto_test.py` | 全自动 E2E 回归（13 用例；**唯一允许的端到端验证入口**）|
| `tools/e2e_host/` | E2E 测试宿主（TSF 激活 + 键脚本 + 截图）|
| `tools/build_lexicon.py` | CC-CEDICT → 双引擎词库 |
| `install_tip.ps1` | 部署（版本名 DLL + 注册 + launcher 重启）|
| `package.ps1` | 构建+测试+打包 zip |

---

## 2. 协议不变量（engine.v0，违反=严重缺陷）

1. **行数守恒**：客户端每发一行，恰收一行。重生后引擎的 init 回复必须被 launcher 吃掉。
2. **显式空候选**：组词中候选变为空时**必须发送 `candidates: []`**（否则候选窗残留）。【台账 B3】
3. **init 语义**：`init` 重置引擎会话状态；TIP 每次（重）激活后首个按键前需 init。
4. **一请求一响应**：引擎不得合并/拆分响应行（launcher 按 FIFO 队列配对）。
5. **Tab 容差**：不支持整句的引擎对 Tab 穿透（consumed=false）。
6. 响应必须含 `ok`；引擎内部异常 → `ok:false` 且进程不崩。

---

## 3. 部署布局与约束

```
C:\Program Files (x86)\glm-ime\
  x64\glm-ime-tip-<时间戳>.dll   # 版本名部署（旧文件被 TSF 锁定时互不影响）
  glm-launcher.exe  glm-engine-rs.exe  lexicon.json
```

| 约束 | 说明 |
|---|---|
| 提权 | `regsvr32` 与写安装目录需要管理员（UAC）|
| **静态 CRT** | TIP 必须 /MT：宿主目录旧版 msvcp140 会劫持 /MD 依赖（台账 B1）|
| DLL 锁定 | TSF 加载后旧 DLL 不可覆盖 → 版本名部署；卸载用 PendingFileRenameOperations |
| 自启 | HKCU Run（HKLM Run 可能被安全软件锁定）|
| 日志 | `%LOCALAPPDATA%\glm-ime\logs\`：TipLog-<pid>/launcher.log（**诊断用，热路径禁止写日志**）|

---

## 4. 问题台账

状态：✅已修 / 🔧待修 / 📋待办

| ID | 现象 | 根因 | 修复/方案 | 状态 | 验证 |
|---|---|---|---|---|---|
| B1 | 切输入法致 Snipaste 闪退 | 宿主目录旧版 msvcp140 劫持 /MD | 静态 CRT /MT | ✅ | WER 无新崩溃+e2e |
| B2 | 记事本打字闪退 | commit 后空 composition 动作 → `setCompositionCursor` 空指针 | 组词结束跳过+libIME 补守卫 | ✅ | WER+map 定位 |
| B3 | `csssss` 候选窗残留 | 候选变空时引擎不发送任何动作 | 显式 `candidates: []` | ✅ | auto_test csssss_stages |
| B4 | 候选窗背景恒白 | FillSolidRect(ExtTextOut 技巧)无效 + Rectangle 白刷涂白 | FillRect+NULL_BRUSH | ✅ | 像素断言 dark 88% |
| B5 | 数字选词失效（测试暴露）| e2e_host 键脚本漏数字字符 | host 补 0-9 | ✅ | auto_test digit1 |
| B6 | 切输入法很卡（按键卡 5s）| 引擎无响应时全链无超时自愈 | launcher 2s 条件超时+请求重放；TIP 4s 退避 | ✅ | 闲时零日志+重放实测 |
| B7 | launcher 假死（全连接 BUSY）| connect Err 仍 spawn 未连接实例 → 阻塞泄漏至上限 | 丢弃重建，绝不 spawn 未连接实例 | ✅ | 代码审查+重启恢复 |
| B8 | 空闲期 launcher 自我重生 | stdout 分支无条件 2s 超时（空闲引擎本无输出）| 仅 pending>0 时启用超时 | ✅ | 空闲 6s 零日志 |
| B9 | 嵌套同步 EditSession 静默失败 | libIME Start/EndComposition 内部自建会话 | session 化实现（editCookie 直用）| ✅ | e2e PASS |
| B10 | 提权管道 DACL 拒绝过滤令牌 | 默认 DACL 绑定完整令牌 | SDDL `D:(A;;GRGW;;;WD)` | ✅ | 连接实测 |
| B11 | x86 构建 LNK2001 | __stdcall 修饰名与导出不匹配 | 架构感知 def 别名 | ✅ | 双架构构建 |
| B12 | **测试宿主破坏性**（抢前台/进程风暴/需重启组件）| e2e_host 依赖前台且流程粗暴 | **改造为非侵入式**（见 §6.3）| 🔧 | - |
| B13 | 候选质量（生僻字/字母词条）| CC-CEDICT 全量 | GB2312 过滤 | ✅ | 候选抽查 |
| B14 | 卡顿修复变更集**未提交** | 即兴调试产物 | 按 §5 流程提交（变更集 C1）| 📋 | git status |
| B15 | 多候选窗尺寸/DPI 长期适配 | 未系统处理 | M6 计划 | 📋 | - |
| B16 | x86 部署缺失 | 仅注册 x64 | M6 计划（32 位应用需要时）| 📋 | - |

---

## 5. 开发流程（**强制**）

每个变更按以下步骤，**不得跳步**：

1. **登记**：在 §4 台账新增/更新条目（现象、根因假设）。
2. **影响面评估**（每条变更必须书面回答）：
   - 动哪些组件（TIP/launcher/engine/测试/部署）？
   - 对**用户系统**的影响：是否弹 UAC？是否动前台/桌面/注册表？何时执行（**必须用户确认时间窗口**）？
3. **实现**：最小改动；改动即更新相关注释（引用台账 ID，如 `// fix B3`）。
4. **自测**（**测试代码先自测**）：
   - 测试工具自身先用最小输入验证（如先跑单用例）**再**跑全量；
   - **禁止**在用户桌面做破坏性验证（见 §6.3）。
5. **回归**：`python tools/auto_test.py` 必须 **13/13**；引擎改动另跑 `compare.py` + `test_m3.py`。
6. **提交**：一次变更一个提交，信息含台账 ID；**先提交再部署**。
7. **部署**：`install_tip.ps1`（管理员，用户确认后）；部署后**自动验证**：管道连接 + `auto_test.py` 冒烟。
8. **回填台账**：状态与验证证据。

### 调试纪律
- 日志：诊断日志只允许低频路径（激活/错误/SEH）；**热路径（每次按键/重绘）禁止写文件**。临时探针用完即删（提交前检查）。
- 排障优先级：日志（TipLog/launcher.log）→ WER 记录 → map 文件定位 → 复现用例。

---

## 6. 测试体系

### 6.1 分层
| 层 | 入口 | 覆盖 |
|---|---|---|
| 单元/协议 | `tools/compare.py`（golden 11 例双引擎）| 按键语义/词库/自学习 |
| 单元 | `tools/test_m3.py` | 自学习/整句/Tab 容差 |
| 集成 | `tools/test_m1.py` | 管道→launcher→引擎（含崩溃重生/多会话）|
| 端到端 | `tools/auto_test.py` | 13 用例：上屏/边界/候选窗像素断言/窗口可见性 |

### 6.2 新增用例规则
- 有 bug 报告 → **先加到 `auto_test.py` 的 CASES**（复现）→ 修 → 回归；
- 断言必须**客观可判定**（文本精确匹配、像素占比、SHOT 序号集合），禁止"肉眼确认"。

### 6.3 e2e_host 非侵入化需求（B12，改造中）
**当前问题**：依赖抢前台（AttachThreadInput），且历史调试中出现过进程风暴/重启 explorer 等破坏性操作——**禁止再发生**。
**约束**：
- 测试期间**不得**：重启 explorer/桌面组件、杀非本测试进程、写用户文档窗口；
- 抢前台仅允许"温和重试+失败即跳过"，**失败时报告 FOCUS=0 并退出**（不硬刚）；
- 批量测试（auto_test）**仅在用户空闲时由用户明确启动**；后续改造方向：独立测试账户或手动置前模式（`--manual-front`）。

---

## 7. 路线图（M6+）

1. **B12 测试非侵入化**（高优先：流程安全）
2. B14 变更集提交与 v0.3 版本标记
3. 真·联想（commit 后预测；需 TIP 异步接收改造——协议 v1 push 通道）
4. 词库二期（词频数据源接入，替代长度启发式）
5. B15 多屏/DPI 候选窗适配；B16 x86 部署
6. 安装器（NSIS 或 zip+脚本固化）

---

## 8. 待办清单（等用户确认）

- [ ] **C1**：提交当前未提交的卡顿修复（launcher 自愈/条件超时 + TIP 退避/日志瘦身），并按 §5.6 补 CHANGELOG
- [ ] **C2**：B12 测试宿主非侵入化改造（先出方案，用户确认后实施）
- [ ] **C3**：确认输入法当前体验（是否仍卡）——如仍卡，按 §5 流程排障（不再直接动系统）
