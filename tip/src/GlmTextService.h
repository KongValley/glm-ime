// glm-ime TIP —— TextService 薄壳。
// 原则（教训 #5）：本类零输入逻辑 —— 按键→管道→动作回填，全部智能在引擎进程。
// 语义锁定 proto/engine.v0.md；libIME 负责全部 TSF/COM 样板。
#pragma once
#include <Windows.h>
#include "TextService.h"

namespace Ime {
class ImeModule;
class CandidateWindow;
}

namespace glm {
struct EngineActions;

class GlmTextService : public Ime::TextService {
public:
    GlmTextService(Ime::ImeModule* module);
    virtual ~GlmTextService();

protected:
    void onActivate() override;
    void onDeactivate() override;
    bool filterKeyDown(Ime::KeyEvent& keyEvent) override;
    bool onKeyDown(Ime::KeyEvent& keyEvent, Ime::EditSession* session) override;
    bool onKeyDownImpl(Ime::KeyEvent& keyEvent, Ime::EditSession* session);
    bool filterKeyDownImpl(Ime::KeyEvent& keyEvent);
    void onCompositionTerminated(bool forced) override;
    void onKeyboardStatusChanged(bool opened) override;

private:
    struct Theme {
        std::wstring font = L"Microsoft YaHei UI";
        int fontSize = 20;
        int candPerRow = 1;
        COLORREF bg = RGB(24, 26, 32), text = RGB(232, 234, 240);
        COLORREF selKey = RGB(120, 170, 255);
        COLORREF selBg = RGB(46, 52, 64), selText = RGB(255, 255, 255);
        COLORREF border = RGB(70, 74, 88);
        bool loaded = false;
    };
    void loadTheme();
    void applyThemeTo(Ime::CandidateWindow* win, Ime::EditSession* session);
    void applyActions(Ime::EditSession* session, const EngineActions& actions);
    void startCompositionInSession(Ime::EditSession* session);
    void endCompositionInSession(Ime::EditSession* session, const wchar_t* finalText, int len);
    void positionCandidateWindow(Ime::EditSession* session);
    void clearState();

    Ime::CandidateWindow* candidateWindow_ = nullptr;
    Theme theme_;
    bool composing_ = false;
    bool englishMode_ = false; // 中英切换（M2：Shift 切换，M3 移到引擎侧协议）
};

} // namespace glm
