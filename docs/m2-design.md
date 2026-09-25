# M2 设计决策记录：C++ TSF TIP

日期：2026-09-25 · 状态：方案定案，实施中

## 选型结论

**vendor libIME2（LGPL-2.1，动态链接）+ 自写 GlmTextService 薄壳（BSD/MIT 风格自有代码）+ CMake**

评估过的三条路：

| 路线 | 结论 |
|---|---|
| 裁剪 DIME（BSD-3） | ❌ 弃。DIME 支持四种码表+注音+短语，按键语义（顶功/wildcard/Dayi 特例/简繁转换）钉死在 CCompositionProcessorEngine 与 KeyHandler 里，接缝不薄；裁掉的历史包袱比引入的还多 |
| 微软 SampleIME 直改 | ❌ 弃。代码更老（Win8 时代），无注册器现成品，DIME 是它的现代化分叉 |
| **libIME2 薄壳** | ✅ 选。libIME2 把 ITfTextInputProcessor/KeyEventSink/EditSession/候选窗包装成 C++ 类；PIMETextService.cpp 是"薄壳+管道客户端"的完整范本，抄薄它对接 glm-launcher |

许可：libIME2 为 LGPL-2.1，以动态库/独立 DLL 形态链接，自有代码闭源合规。

## 架构（对接 M1 交付物）

```
glm-ime-tip.dll (C++17, x86+x64)
  ├─ libIME2 (vendored, LGPL)        TSF 包装：CTextInputProcessor/KeyEventSink/CCandidateWindow
  ├─ PipeClient                       命名管道客户端 → \\.\pipe\<user>\glm-ime\launcher
  │    协议：engine.v0 JSON 行（init/key/reset，动作回包）
  └─ GlmTextService : libIME::CTextInputProcessor
       onKeyDown(keyEvent) → pipe key → actions:
         composition{text,cursor} → libIME composition set
         candidates{list}         → CCandidateWindow set + show
         commit{text}             → libIME commitText
         consumed=false           → 不吃键，透传
```

## 关键映射表（engine.v0 动作 → libIME API）

| 动作 | libIME 调用 |
|---|---|
| composition(text,cursor) | _composition->setText + setCursor（StartComposition/EndComposition 管理）|
| candidates(list) | _candidateWindow->setCandidateList / setShowCandidates |
| commit(text) | _composition->commitText(text) |
| consumed=false | return 不吃 |

按键语义（字母追加/空格首选/数字选词/Backspace/Enter 原样/Esc 清空）**全部在引擎侧**，
TIP 不实现任何输入逻辑——这是与 PIME 最大的不同（PIME 语义在 Python，我们在引擎，
TIP 纯转发，协议 engine.v0 已锁死行为）。

## 注册

- 复用 PIME 模式：`DllRegisterServer` 扫 `input_methods/*/ime.json`（guid/locale/name）
- locale `zh-Hans-CN`；CLSID 独立新 GUID；x86/x64 双 DLL
- 安装器：`SysWOW64\regsvr32`（x86）+ `System32\regsvr32`（x64）（教训 #2）
- Launcher 由 HKLM Run 自启（M1 已交付单实例守卫）

## 工程布局

```
tip/
  CMakeLists.txt            x64 优先，Win32 随后
  libIME2/                  vendored (EasyIME/libIME2 @ master)
  src/
    dllmain.cpp             DllGetClassObject/DllRegisterServer/DllUnregisterServer
    GlmImeModule.cpp/.h     模块：注册 + 引导 GlmTextService
    GlmTextService.cpp/.h   ITfTextInputProcessor + 键盘/组合/候选
    PipeClient.cpp/.h       命名管道客户端（engine.v0，JSON 行，一请求一响应，5s 超时）
    glm-ime-tip.def         导出
    ime.json                {guid, locale: zh-Hans-CN, name: glm-ime}
```

## 验收标准（M2 完成定义）

1. x64 DLL 构建通过，regsvr32 注册成功（管理员）
2. Settings 中文(简体)出现 "glm-ime" 键盘
3. 记事本真键盘：nihao+空格 → 你好；数字选词；Esc 清空；Shift 英文穿透
4. 断 launcher（kill glm-launcher.exe）→ TIP 不崩宿主应用，重启 launcher 后恢复

## 教训沿用（README #1-#5）

构建必须 ASCII 路径（F:\glm-ime ✓）；双 regsvr32；卸载用 PendingFileRenameOperations；
TIP 壳零逻辑零 panic 路径；引擎崩 → launcher 重生（M1 D4），TIP 侧管道断开只清空状态不崩。
