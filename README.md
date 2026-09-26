# glm-ime

自用 Windows 输入法。**glm** = 拼音首字母风格命名，本地内核 + LLM 增强。

> **开发规范（强制）**：所有修复与开发按 [`docs/TECH.md`](docs/TECH.md) 执行——
> 问题先登记台账 → 评估影响面（含对用户系统的影响）→ 实现 → 自动化验证(13/13) → 提交 → 部署。
> 禁止未经确认的破坏性测试（抢前台/动桌面/重启系统组件）。

## 架构

```
┌────────────────────────────────────────────────┐
│ glm-ime-tip.dll (C++ TSF TIP, x86+x64)         │  薄壳：按键捕获 / composition / 自绘候选窗
└──────────────┬─────────────────────────────────┘
               │ named pipe  \\.\pipe\<user>\glm-ime\launcher
┌──────────────▼─────────────────────────────────┐
│ glm-ime-launcher (Rust 常驻单例)                │  会话复用 / 引擎托管 / 崩溃重启
└──────────────┬─────────────────────────────────┘
               │ stdio JSON Lines（协议 engine.v0，引擎无关）
     ┌─────────┴─────────┐
     ▼                   ▼
┌──────────────┐  ┌──────────────────┐
│ engine-py    │  │ engine-rs        │
│ 迭代快/LLM生态 │  │ 极速/单二进制/低内存│
└──────────────┘  └──────────────────┘
```

- **TSF 壳（C++）**：唯一职责 = 按键进管道、动作出上屏。所有智能在引擎进程。
- **双引擎对比**：同协议、同 golden 测试集、同基准跑分，数据说话再定主力引擎。
- **LLM 层**（M3）：整句重排 / 联想 / 纠错 / 风格化；无 API key 或断网时纯本地兜底。

## 目录

```
proto/engine.v0.md   引擎协议规范（两引擎的契约，唯一真相源）
engine-py/           Python 引擎
crates/engine-rs/    Rust 引擎
tools/compare.py     对比 harness：正确性 + 延迟/内存/冷启动
tools/golden/        测试用例（键序列 → 期望动作）
docs/                决策记录
```

## 里程碑

| 里程碑 | 内容 | 验收 |
|---|---|---|
| M0 内核与对比 | 双引擎 + golden + 跑分 | harness 报告：正确性 100% 一致、延迟/内存对比表 |
| M1 服务层 | Rust launcher + 命名管道 | headless 全链测试过（管道→launcher→引擎）|
| M2 系统集成 | C++ TSF DLL + 候选窗 + 安装 | 真键盘在任意应用打出中文 |
| M3 LLM 增强 | 重排/联想/纠错 + 兜底 | 断网可用；有 key 时候选质量提升 |
| M4 交付 | 安装包 + 更新 | 一键装/卸 |

## 已知教训（来自 PIME 实战，复用）

1. 构建路径必须 ASCII（中文路径卡 Corrosion/cargo metadata）→ 本仓库放 `F:\glm-ime`。
2. 32 位 DLL 用 `SysWOW64\regsvr32.exe` 注册，64 位用 `System32`。
3. TSF DLL 被焦点进程锁死，卸载需 `PendingFileRenameOperations`。
4. 后端 stdout 必须纯协议行；诊断走 stderr / 日志文件。
5. TSF DLL 死会拖垮宿主应用 → 壳内零逻辑、零 panic 路径。

## 状态

| 里程碑 | 状态 |
|---|---|
| M0 内核与对比 | ✅ 双引擎 10/10 golden 一致（`docs/m0-comparison.md`）|
| M1 服务层 | ✅ 管道→launcher→引擎全链 + 崩溃重生（`docs/m1-design.md`）|
| M2 系统集成 | ✅ x86+x64 TIP 构建注册、e2e_host 实测 `nihao`+空格→`你好`（`docs/m2-design.md`）|
| M3 LLM 增强 | ✅ Tab 整句（LLM+DP 兜底）/ 用户词典自学习，测试 4/4（`tools/test_m3.py`）|
| M4 交付 | ✅ `package.ps1` 一键构建+测试+zip；升级 = 重跑 `install_tip.ps1` |

### M2 实测教训（新增）

6. **libIME 的 startComposition/endComposition 内部自建 TF_ES_SYNC EditSession；
   在 OnKeyDown 的 EditSession 回调内调用 = 嵌套同步会话被 TSF 静默拒绝**。
   解法：抄其逻辑改为使用 onKeyDown 传入 session 的 editCookie 直接操作
   （`GlmTextService::startCompositionInSession/endCompositionInSession`）。
   vendored libIME2 的 `composition_` 成员相应从 private 移到 protected。
7. 提权 shell 启动的 launcher，其管道默认 DACL 会拒绝非提权进程（UAC 过滤令牌）——
   管道必须显式 DACL（SDDL `D:(A;;GRGW;;;WD)`；手搓 SD 需 Owner+ACL 且易错）。
8. 测试宿主必须泵消息：SendInput 的输入在无 GetMessage 循环的线程上永不分发。

### 自动化验证（零人工）

```bash
python tools/auto_test.py     # 10 项断言：上屏正确性/数字选词/ESC/退格/词库/候选窗像素断言
```
- 单进程 suite 模式（`e2e_host --suite`）：一次前台获取跑全部用例
- 候选窗**截图 + 像素级断言**（BitBlt 真实屏幕，内部区域采样判定主题）
- 宿主进程独立黑匣子日志：`%LOCALAPPDATA%\glm-ime\logs\TipLog-<pid>.log`

### 运行布局

```
C:\Program Files (x86)\glm-ime\
  x64\glm-ime-tip.dll     # TSF TIP（版本名部署，注册表指向）
  glm-launcher.exe          # 常驻单例（HKLM Run 自启）
  glm-engine-rs.exe         # 默认引擎
  lexicon.json
```

安装/升级：管理员运行 `install_tip.ps1`（regsvr32 + 自启 + 启动 launcher）；
全量构建打包：`powershell -File package.ps1` → `dist/glm-ime-<时间戳>.zip`。
TSF 键盘列表：设置 → 中文(简体) → 键盘 → "glm-ime"。

### 快捷键与 LLM 配置

- `nihao` + `空格` → 你好；`1-9` 选词；`Enter` 原样上屏；`Esc` 取消；`Shift` 中英
- `Tab`（engine-py 引擎）= 整句生成：配置 `%LOCALAPPDATA%\glm-ime\config.json`
  的 `llm` 节（OpenAI 兼容）走 LLM，无 key/断网时本地拼音切分兜底
- 自学习：提交过的词自动写入 `%LOCALAPPDATA%\glm-ime\user_lexicon.json`
