// glm-ime TIP —— TextService 薄壳实现。见 GlmTextService.h。
// 原则：本类零输入逻辑。按键→管道（engine.v0）→动作回填。语义锁定 proto/engine.v0.md。
//
// 关键坑（M2 实测）：libIME 的 startComposition/endComposition 内部自建
// TF_ES_SYNC EditSession；在 OnKeyDown 的 EditSession 回调内调用= 嵌套同步
// 会话 = TSF 静默失败（sessionResult 被忽略）。因此本类把这两段逻辑改为
// 直接使用 onKeyDown 传入 session 的 editCookie（startCompositionInSession 等）。
#include "json.hpp"
#include "GlmTextService.h"

// dllmain.cpp 定义（全局命名空间）
extern HMODULE g_selfModule;
#include "PipeClient.h"
#include "ImeModule.h"
#include "CandidateWindow.h"
#include "EditSession.h"
#include "KeyEvent.h"
#include <cstdio>
#include <cstdarg>

static void svcLog(const char* fmt, ...) {
    char path[MAX_PATH];
    ::GetEnvironmentVariableA("GLM_TIP_LOG", path, MAX_PATH);
    FILE* f = nullptr;
    fopen_s(&f, path, "a");
    if (!f) return;
    va_list args; va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

using namespace Ime;

namespace glm {

GlmTextService::GlmTextService(ImeModule* module) : TextService(module) {
}

GlmTextService::~GlmTextService() {
    if (candidateWindow_) {
        candidateWindow_->Release(); // 壳接管初始引用（构造即 1）
        candidateWindow_ = nullptr;
    }
}

void GlmTextService::onActivate() {
    wchar_t modPath[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, modPath, MAX_PATH); // 宿主 exe
    extern HMODULE g_selfModule;
    wchar_t selfPath[MAX_PATH] = {};
    ::GetModuleFileNameW(g_selfModule, selfPath, MAX_PATH);
    svcLog("[%p] onActivate host=%ls dll=%ls", (void*)this, modPath, selfPath);
    TextService::onActivate();
    // 手动激活路径（测试宿主/个别应用）不会置键盘开关，显式打开
    setKeyboardOpen(true);
    PipeClient::instance().init(); // 引擎会话 init；失败不阻断激活，下次按键重试
    englishMode_ = false;
}

void GlmTextService::onDeactivate() {
    clearState();
    TextService::onDeactivate();
}

void GlmTextService::clearState() {
    composing_ = false;
    if (candidateWindow_)
        candidateWindow_->hide();
}

void GlmTextService::onKeyboardStatusChanged(bool opened) {
    if (!opened)
        clearState();
}

void GlmTextService::onCompositionTerminated(bool forced) {
    // TSF 强制终止组合（焦点切换等）：清壳内状态；composition_ 由 libIME 清理
    composing_ = false;
    if (candidateWindow_)
        candidateWindow_->hide();
}

bool GlmTextService::filterKeyDown(KeyEvent& keyEvent) {
    // 英文模式全穿透；非组词时回车/退格/空格穿透（避免吃掉应用按键）
    if (englishMode_)
        return false;
    if (!composing_) {
        UINT c = keyEvent.keyCode();
        if (c == VK_RETURN || c == VK_BACK || c == VK_SPACE)
            return false;
    }
    return true;
}

bool GlmTextService::onKeyDown(KeyEvent& keyEvent, EditSession* session) {
    UINT code = keyEvent.keyCode();
    // Shift 单独按 = 中英切换（M2 简化版；M3 移入引擎协议）
    if (code == VK_SHIFT) {
        englishMode_ = !englishMode_;
        clearState();
        return true;
    }

    EngineActions actions;
    bool ok = PipeClient::instance().sendKey(code,
        keyEvent.isChar() ? (WCHAR)keyEvent.charCode() : 0, 0, actions);
    if (!ok) {
        // 管道故障：清壳内状态、不吃键（宿主应用不受伤）；下次按键自动重连
        svcLog("[%p] sendKey FAILED", (void*)this);
        clearState();
        return false;
    }
    applyActions(session, actions);
    return actions.consumed;
}

// 在【传入的】EditSession 内启动组词（避免嵌套同步会话被 TSF 拒绝）
// 逻辑抄自 libIME TextService::startComposition，改用 session->editCookie()
void GlmTextService::startCompositionInSession(EditSession* session) {
    ITfContext* context = session->context();
    TfEditCookie ec = session->editCookie();
    ComPtr<ITfContextComposition> contextComposition;
    if (FAILED(context->QueryInterface(IID_ITfContextComposition, (void**)&contextComposition)))
        return;
    ComPtr<ITfRange> range;
    ComPtr<ITfInsertAtSelection> insertAtSelection;
    if (SUCCEEDED(context->QueryInterface(IID_ITfInsertAtSelection, (void**)&insertAtSelection))) {
        // 取当前插入点（只查询，不插入）
        insertAtSelection->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, NULL, 0, &range);
    }
    if (range) {
        composition_ = nullptr;
        if (contextComposition->StartComposition(ec, range, (ITfCompositionSink*)this, &composition_) == S_OK) {
            // 官方样例要求重置选区（StartComposition 可能改变 range）
            TF_SELECTION selection;
            selection.range = range;
            selection.style.ase = TF_AE_NONE;
            selection.style.fInterimChar = FALSE;
            context->SetSelection(ec, 1, &selection);
            composing_ = true;
        }
    }
}

// 在【传入的】EditSession 内结束组词并上屏文本
// 逻辑抄自 libIME TextService::endComposition，改用 session->editCookie()
void GlmTextService::endCompositionInSession(EditSession* session, const wchar_t* finalText, int len) {
    ITfContext* context = session->context();
    TfEditCookie ec = session->editCookie();
    if (composition_) {
        // 提交文本：组合范围替换为定稿文本
        if (finalText && len > 0)
            setCompositionString(session, finalText, len);
        ComPtr<ITfRange> compositionRange;
        if (composition_->GetRange(&compositionRange) == S_OK) {
            ComPtr<ITfProperty> dispAttrProp;
            if (context->GetProperty(GUID_PROP_ATTRIBUTE, &dispAttrProp) == S_OK)
                dispAttrProp->Clear(ec, compositionRange);
            TF_SELECTION selection;
            ULONG selectionNum = 0;
            if (context->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection, &selectionNum) == S_OK) {
                selection.range->ShiftEndToRange(ec, compositionRange, TF_ANCHOR_END);
                selection.range->Collapse(ec, TF_ANCHOR_END);
                context->SetSelection(ec, 1, &selection);
                selection.range->Release();
            }
        }
        composition_->EndComposition(ec);
        onCompositionTerminated(false);
        composition_ = nullptr;
        composing_ = false;
    }
}

// 主题：DLL 同目录 theme.json（可选）。缺省内置深色主题。
// {"font":"Microsoft YaHei UI","fontSize":20,"candPerRow":1,
//  "bg":[24,26,32],"text":[232,234,240],"selKey":[120,170,255],
//  "selectedBg":[46,52,64],"selectedText":[255,255,255],"border":[70,74,88]}
static std::wstring tipUtf8ToWide(const std::string& u) {
    if (u.empty()) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, u.data(), (int)u.size(), nullptr, 0);
    std::wstring w(n, 0);
    ::MultiByteToWideChar(CP_UTF8, 0, u.data(), (int)u.size(), w.data(), n);
    return w;
}

static bool readThemeFile(const std::wstring& path, nlohmann::json& out) {
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"rb");
    if (!f) return false;
    std::string buf;
    char tmp[4096];
    size_t n = 0;
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) buf.append(tmp, n);
    fclose(f);
    try { out = nlohmann::json::parse(buf); return out.is_object(); }
    catch (...) { return false; }
}

static COLORREF rgbFromJson(const nlohmann::json& j, const char* key, COLORREF def) {
    if (!j.contains(key)) return def;
    auto& a = j[key];
    if (!a.is_array() || a.size() < 3) return def;
    return RGB(a[0].get<int>(), a[1].get<int>(), a[2].get<int>());
}

void GlmTextService::loadTheme() {
    if (theme_.loaded) return;
    wchar_t dllPath[MAX_PATH] = {};
    ::GetModuleFileNameW(g_selfModule, dllPath, MAX_PATH);
    std::wstring dir = dllPath;
    size_t pos = dir.find_last_of(L'\\');
    if (pos != std::wstring::npos) dir.resize(pos);
    std::wstring path = dir + L"\\theme.json";
    nlohmann::json j;
    if (readThemeFile(path, j)) {
        if (j.contains("font")) theme_.font = tipUtf8ToWide(j["font"].get<std::string>());
        theme_.fontSize = j.value("fontSize", theme_.fontSize);
        theme_.candPerRow = j.value("candPerRow", theme_.candPerRow);
        theme_.bg = rgbFromJson(j, "bg", theme_.bg);
        theme_.text = rgbFromJson(j, "text", theme_.text);
        theme_.selKey = rgbFromJson(j, "selKey", theme_.selKey);
        theme_.selBg = rgbFromJson(j, "selectedBg", theme_.selBg);
        theme_.selText = rgbFromJson(j, "selectedText", theme_.selText);
        theme_.border = rgbFromJson(j, "border", theme_.border);
    }
    theme_.loaded = true;
}

void GlmTextService::applyThemeTo(CandidateWindow* win, EditSession* session) {
    loadTheme();
    HFONT f = ::CreateFontW(
        -theme_.fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, theme_.font.c_str());
    if (f) win->setFont(f);
    win->setCandPerRow(theme_.candPerRow);
    win->setTextColor(theme_.text);
    win->setSelKeyColor(theme_.selKey);
    win->setSelectedColors(theme_.selBg, theme_.selText);
    win->setBackgroundColor(theme_.bg);
    win->setBorderColor(theme_.border);
    win->recalculateSize();
}

void GlmTextService::applyActions(EditSession* session, const EngineActions& actions) {
    for (auto& act : actions.actions) {
        switch (act.type) {
        case EngineActions::Action::Composition: {
            if (!composing_ && !act.text.empty())
                startCompositionInSession(session);
            if (composing_)
                setCompositionString(session, act.text.c_str(), (int)act.text.size());
            if (composing_ && act.text.empty()) {
                // 空串 = 结束组词
                endCompositionInSession(session, L"", 0);
            }
            setCompositionCursor(session, act.cursor);
            break;
        }
        case EngineActions::Action::Candidates: {
            if (!candidateWindow_) {
                candidateWindow_ = new CandidateWindow(this, session);
                applyThemeTo(candidateWindow_, session);
            }
            candidateWindow_->clear();
            const wchar_t* selKeys = L"123456789";
            int n = 0;
            for (auto& item : act.list) {
                candidateWindow_->add(item, selKeys[n]);
                if (++n >= 9) break;
            }
            candidateWindow_->recalculateSize();
            positionCandidateWindow(session);
            candidateWindow_->show();
            break;
        }
        case EngineActions::Action::Commit: {
            // TSF 提交语义：组合范围替换为定稿文本，EndComposition 后落盘
            endCompositionInSession(session, act.text.c_str(), (int)act.text.size());
            if (candidateWindow_)
                candidateWindow_->hide();
            break;
        }
        default:
            break;
        }
    }
}

void GlmTextService::positionCandidateWindow(EditSession* session) {
    RECT rect;
    if (compositionRect(session, &rect)) {
        int x = rect.left;
        int y = rect.bottom;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        int w = 0, h = 0;
        candidateWindow_->size(&w, &h);
        ::SetWindowPos(candidateWindow_->hwnd(), HWND_TOPMOST, x, y, w, h,
                       SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

} // namespace glm
