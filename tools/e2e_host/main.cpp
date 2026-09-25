// glm-ime 自动化 E2E 测试宿主 v2
// 用法: e2e_host.exe --keys "nihao<SP>nihao" --out result.txt --shot-prefix shot
// 键脚本: 小写字母 / <SP>空格 <ENT>回车 <ESC> <BS>退格 <TAB> <SHOT>截图候选窗
// 输出: result.txt = "文本|SHOT:<file1>,<file2>|CAND:<window_class_list>"
#include <windows.h>
#include <msctf.h>
#include <objbase.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <array>
#include <atomic>
#include <thread>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "uuid.lib")

static const CLSID kTipClsid =
    { 0x7e4a6b1d, 0x93c2, 0x4e57, { 0x9a, 0x51, 0x3b, 0x8f, 0x2d, 0x6c, 0x0a, 0x44 } };
static const GUID kProfileGuid =
    { 0x5a9e2c4b, 0x1d6f, 0x4a83, { 0xb7, 0xe2, 0x9c, 0x4d, 0x8f, 0x1a, 0x6e, 0x3b } };

static HWND g_edit = nullptr;
static std::atomic<bool> g_done{false};
static void grabForeground();

// ---------- 候选窗截图（BMP） ----------
struct ShotInfo { std::string file; int w = 0, h = 0; };

static bool saveBmp32(const char* path, HBITMAP hbm, int w, int h) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<unsigned char> pixels((size_t)w * h * 4);
    HDC dc = ::GetDC(nullptr);
    int got = ::GetDIBits(dc, hbm, 0, h, pixels.data(), &bi, DIB_RGB_COLORS);
    ::ReleaseDC(nullptr, dc);
    if (!got) return false;
    FILE* f = nullptr;
    fopen_s(&f, path, "wb");
    if (!f) return false;
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + (DWORD)pixels.size();
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
    fwrite(pixels.data(), 1, pixels.size(), f);
    fclose(f);
    return true;
}

// 枚举本进程全部顶层窗口，截图所有非 EDIT/非本窗的可见窗口（即候选窗）
static std::vector<ShotInfo> captureCandidateWindows(const std::string& prefix, int seq) {
    std::vector<ShotInfo> shots;
    struct Ctx { std::string prefix; int seq; std::vector<ShotInfo>* shots; } ctx{prefix, seq, &shots};
    ::EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* c = (Ctx*)lp;
        DWORD pid = 0;
        ::GetWindowThreadProcessId(hwnd, &pid);
        if (pid != ::GetCurrentProcessId()) return TRUE;
        if (!::IsWindowVisible(hwnd)) return TRUE;
        wchar_t cls[128] = {};
        ::GetClassNameW(hwnd, cls, 128);
        if (_wcsicmp(cls, L"LibImeWindow") != 0) return TRUE;
        RECT rc{};
        ::GetWindowRect(hwnd, &rc);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0) return TRUE;
        // 路1：PrintWindow(PW_RENDERFULLCONTENT) —— 窗口自绘，不受位置/遮挡/DPI 影响
        HDC scr = ::GetDC(nullptr);
        HDC mem = ::CreateCompatibleDC(scr);
        HBITMAP bmp = ::CreateCompatibleBitmap(scr, w, h);
        HGDIOBJ old = ::SelectObject(mem, bmp);
        ::PrintWindow(hwnd, mem, 2 /*PW_RENDERFULLCONTENT*/);
        char file[MAX_PATH];
        snprintf(file, MAX_PATH, "%s_%d_%d_pw.bmp", c->prefix.c_str(), c->seq, (int)c->shots->size());
        if (saveBmp32(file, bmp, w, h))
            c->shots->push_back({file, w, h});
        // 路2：BitBlt 屏幕真实像素
        ::BitBlt(mem, 0, 0, w, h, scr, rc.left, rc.top, SRCCOPY | CAPTUREBLT);
        snprintf(file, MAX_PATH, "%s_%d_%d_bb.bmp", c->prefix.c_str(), c->seq, (int)c->shots->size());
        if (saveBmp32(file, bmp, w, h))
            c->shots->push_back({file, w, h});
        ::SelectObject(mem, old);
        ::DeleteObject(bmp);
        ::DeleteDC(mem);
        ::ReleaseDC(nullptr, scr);
        return TRUE;
    }, (LPARAM)&ctx);
    return shots;
}

// ---------- 输入 ----------
static bool g_usePost = false;   // --post：PostMessage 投递（免前台依赖；TSF 消息钩子照常拦截）

static void typeKey(WORD vk) {
    if (g_usePost) {
        UINT sc = ::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        LPARAM down = (LPARAM)(sc << 16);
        LPARAM up = (LPARAM)((sc << 16) | 0xC0000000u);
        ::PostMessageW(g_edit, WM_KEYDOWN, vk, down);
        ::Sleep(20);
        ::PostMessageW(g_edit, WM_KEYUP, vk, up);
        ::Sleep(25);
        return;
    }
    INPUT in[2] = {};
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = vk;
    in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = vk; in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    ::SendInput(2, in, sizeof(INPUT));
    ::Sleep(45);
}

static std::string g_fullShotPrefix;   // 非空时每次 SHOT 旁路全屏截图
static std::atomic<int> g_shotSeq{0};
static std::vector<ShotInfo> g_shots;
static std::string g_script;
static std::string g_shotPrefix;

static void clearModifiers() {
    const WORD mods[] = { VK_MENU, VK_SHIFT, VK_CONTROL, VK_LWIN, VK_RWIN };
    for (WORD vk : mods) {
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk;
        in.ki.dwFlags = KEYEVENTF_KEYUP;
        ::SendInput(1, &in, sizeof(INPUT));
    }
}

static void grabForeground() {
    for (int i = 0; i < 15 && ::GetForegroundWindow() != g_edit; ++i) {
        // SendInput 单发 ALT 键对：真实输入事件使本进程获得前台权利（系统前台锁定策略）
        INPUT alt[2] = {};
        alt[0].type = INPUT_KEYBOARD; alt[0].ki.wVk = VK_MENU;
        alt[1].type = INPUT_KEYBOARD; alt[1].ki.wVk = VK_MENU; alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
        ::SendInput(2, alt, sizeof(INPUT));
        ::Sleep(80);
        DWORD fg = ::GetWindowThreadProcessId(::GetForegroundWindow(), nullptr);
        DWORD my = ::GetCurrentThreadId();
        if (fg) ::AttachThreadInput(my, fg, TRUE);
        ::BringWindowToTop(g_edit);
        ::ShowWindow(g_edit, SW_RESTORE);
        ::SetForegroundWindow(g_edit);
        ::SetActiveWindow(g_edit);
        ::SetFocus(g_edit);
        if (fg) ::AttachThreadInput(my, fg, FALSE);
        ::Sleep(200);
        clearModifiers();   // 清除 ALT 残留，避免后续字符被当组合键
    }
}

static std::atomic<bool> g_focusOk{false};

static void runScript() {
    ::Sleep(800);
    grabForeground();          // 打字前再抢一次（幂等）
    g_focusOk = g_usePost ? true : (::GetForegroundWindow() == g_edit);
    size_t i = 0;
    while (i < g_script.size()) {
        if (g_script[i] == '<') {
            size_t end = g_script.find('>', i);
            if (end != std::string::npos) {
                std::string tok = g_script.substr(i + 1, end - i - 1);
                if (tok == "SP") typeKey(VK_SPACE);
                else if (tok == "ENT") typeKey(VK_RETURN);
                else if (tok == "ESC") typeKey(VK_ESCAPE);
                else if (tok == "BS") typeKey(VK_BACK);
                else if (tok == "TAB") typeKey(VK_TAB);
                else if (tok == "SHOT") {
                    ::Sleep(250);
                    auto s = captureCandidateWindows(g_shotPrefix, g_shotSeq++);
                    g_shots.insert(g_shots.end(), s.begin(), s.end());
                }
                i = end + 1;
                continue;
            }
        }
        char c = g_script[i++];
        if (c >= 'a' && c <= 'z') typeKey((WORD)(c - 'a' + 'A'));
        else if (c >= 'A' && c <= 'Z') typeKey((WORD)c);
        else if (c >= '0' && c <= '9') typeKey((WORD)c);
    }
    ::Sleep(400);
    g_done = true;
}

static std::string g_suitePath;

// suite 模式：单进程内跑全部用例（只抢一次前台，避免反复前台竞争）
// 隔离测试桌面：完全避免用户前台干扰（TSF 每桌面独立，输入投递到本桌面前台）
static HDESK g_origDesk = nullptr;
static HDESK g_testDesk = nullptr;

static bool enterTestDesktop() {
    g_origDesk = ::OpenInputDesktop(0, FALSE, GENERIC_ALL);
    g_testDesk = ::CreateDesktopW(L"glmTestDesk", nullptr, nullptr, 0,
                                  DESKTOP_CREATEWINDOW | DESKTOP_SWITCHDESKTOP | DESKTOP_READOBJECTS |
                                  DESKTOP_WRITEOBJECTS | DESKTOP_ENUMERATE | GENERIC_ALL, nullptr);
    if (!g_testDesk) return false;
    if (!::SetThreadDesktop(g_testDesk)) return false;
    return true;   // 注意：调用方决定是否 SwitchDesktop
}

static void leaveTestDesktop() {
    if (g_origDesk) { ::SwitchDesktop(g_origDesk); ::CloseDesktop(g_origDesk); g_origDesk = nullptr; }
    if (g_testDesk) { ::CloseDesktop(g_testDesk); g_testDesk = nullptr; }
}

static int runSuite(const std::string& suitePath, const std::string& outPath) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    FILE* sf = nullptr;
    fopen_s(&sf, suitePath.c_str(), "rb");
    if (!sf) { printf("FAIL open suite\n"); return 1; }
    std::vector<std::array<std::string, 3>> cases; // name, keys, expect
    {
        std::string content;
        char buf[4096]; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), sf)) > 0) content.append(buf, n);
        fclose(sf);
        size_t pos = 0;
        while (pos < content.size()) {
            size_t eol = content.find('\n', pos);
            std::string line = content.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
            pos = (eol == std::string::npos) ? content.size() : eol + 1;
            if (line.empty() || line[0] == '#') continue;
            auto p1 = line.find("||");
            auto p2 = line.find("||", p1 + 2);
            if (p1 == std::string::npos || p2 == std::string::npos) continue;
            cases.push_back({line.substr(0, p1), line.substr(p1 + 2, p2 - p1 - 2), line.substr(p2 + 2)});
        }
    }
    if (FAILED(::CoInitialize(nullptr))) { printf("FAIL CoInitialize\n"); return 1; }
    g_edit = ::CreateWindowW(L"EDIT", L"", WS_OVERLAPPEDWINDOW | WS_VISIBLE | ES_LEFT,
                             100, 100, 520, 130, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (!g_edit) { printf("FAIL edit\n"); return 1; }
    ITfThreadMgr* tm = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr, (void**)&tm))) return 1;
    TfClientId cid = 0;
    tm->Activate(&cid);
    ITfInputProcessorProfiles* profiles = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER, IID_ITfInputProcessorProfiles, (void**)&profiles))) return 1;
    profiles->ActivateLanguageProfile(kTipClsid, 0x0804, kProfileGuid);
    ::ShowWindow(g_edit, SW_SHOWNORMAL);
    grabForeground();
    DWORD start = ::GetTickCount();
    MSG msg;
    // 等待焦点稳定
    for (int i = 0; i < 40 && ::GetForegroundWindow() != g_edit; ++i) {
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { ::TranslateMessage(&msg); ::DispatchMessageW(&msg); }
        ::Sleep(50);
    }
    bool focused = (::GetForegroundWindow() == g_edit);

    FILE* rf = nullptr; fopen_s(&rf, outPath.c_str(), "wb");
    if (rf) {
        wchar_t fgCls[128] = {}; HWND fgw = ::GetForegroundWindow();
        ::GetClassNameW(fgw, fgCls, 128);
        fprintf(rf, "FOCUS|%d|%ls\n", (int)focused, fgCls);
    }

    for (auto& c : cases) {
        // 复位：清文本 + 重新激活语言档案（触发 TIP Deactivate/Activate → 引擎会话 init 重置缓冲）
        ::SetWindowTextW(g_edit, L"");
        clearModifiers();
        profiles->ActivateLanguageProfile(kTipClsid, 0x0804, kProfileGuid);
        for (int k = 0; k < 8; ++k) {
            while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { ::TranslateMessage(&msg); ::DispatchMessageW(&msg); }
            ::Sleep(20);
        }
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { ::TranslateMessage(&msg); ::DispatchMessageW(&msg); }
        g_script = c[1];
        g_shotSeq = 0;
        std::vector<ShotInfo> shots;
        g_shots.clear();
        std::string prevPrefix = g_shotPrefix;
        g_shotPrefix = outPath + "." + c[0];
        {
            // 同步执行键脚本（主线程，边泵消息边打）
            size_t i = 0;
            while (i < g_script.size()) {
                if (g_script[i] == '<') {
                    size_t end = g_script.find('>', i);
                    if (end != std::string::npos) {
                        std::string tok = g_script.substr(i + 1, end - i - 1);
                        if (tok == "SP") typeKey(VK_SPACE);
                        else if (tok == "ENT") typeKey(VK_RETURN);
                        else if (tok == "ESC") typeKey(VK_ESCAPE);
                        else if (tok == "BS") typeKey(VK_BACK);
                        else if (tok == "TAB") typeKey(VK_TAB);
                        else if (tok == "SHOT") {
                            for (int k = 0; k < 10; ++k) {
                                while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { ::TranslateMessage(&msg); ::DispatchMessageW(&msg); }
                                ::Sleep(50);
                            }
                            if (!g_fullShotPrefix.empty()) {
                                int sw = ::GetSystemMetrics(SM_CXSCREEN);
                                int sh = ::GetSystemMetrics(SM_CYSCREEN);
                                HDC src = ::GetDC(nullptr);
                                HDC mem = ::CreateCompatibleDC(src);
                                HBITMAP bmp = ::CreateCompatibleBitmap(src, sw, sh);
                                HGDIOBJ old = ::SelectObject(mem, bmp);
                                ::BitBlt(mem, 0, 0, sw, sh, src, 0, 0, SRCCOPY);
                                char ff[MAX_PATH];
                                snprintf(ff, MAX_PATH, "%s_full_%d.bmp", g_fullShotPrefix.c_str(), g_shotSeq.load());
                                saveBmp32(ff, bmp, sw, sh);
                                ::SelectObject(mem, old); ::DeleteObject(bmp); ::DeleteDC(mem);
                                ::ReleaseDC(nullptr, src);
                            }
                            auto s2 = captureCandidateWindows(g_shotPrefix, g_shotSeq++);
                            shots.insert(shots.end(), s2.begin(), s2.end());
                        }
                        i = end + 1;
                        continue;
                    }
                }
                char ch = g_script[i++];
                if (ch >= 'a' && ch <= 'z') typeKey((WORD)(ch - 'a' + 'A'));
                else if (ch >= 'A' && ch <= 'Z') typeKey((WORD)ch);
                else if (ch >= '0' && ch <= '9') typeKey((WORD)ch);
                for (int k = 0; k < 4; ++k) {
                    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { ::TranslateMessage(&msg); ::DispatchMessageW(&msg); }
                    ::Sleep(5);
                }
            }
        }
        ::Sleep(300);
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { ::TranslateMessage(&msg); ::DispatchMessageW(&msg); }
        wchar_t text[512] = {};
        ::GetWindowTextW(g_edit, text, 512);
        int len = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
        std::string utf8(len, 0);
        ::WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8.data(), len, nullptr, nullptr);
        if (rf) {
            fprintf(rf, "CASE|%s|%s|", c[0].c_str(), utf8.c_str());
            for (size_t i = 0; i < shots.size(); ++i)
                fprintf(rf, "%s%s:%dx%d", i ? "," : "", shots[i].file.c_str(), shots[i].w, shots[i].h);
            fprintf(rf, "\n");
            fflush(rf);
        }
        g_shotPrefix = prevPrefix;
    }
    if (rf) fclose(rf);
    // 交还前台给控制台/桌面，避免下一个测试实例面对"已退出的宿主窗口"而抢不到前台
    if (HWND con = ::GetConsoleWindow()) {
        ::SetForegroundWindow(con);
    } else {
        ::ShowWindow(g_edit, SW_MINIMIZE);
    }
    ::Sleep(150);
    printf("SUITE DONE\n");
    return 0;
}

int main(int argc, char** argv) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    std::string outPath = "result.txt";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--keys" && i + 1 < argc) g_script = argv[++i];
        else if (a == "--out" && i + 1 < argc) outPath = argv[++i];
        else if (a == "--shot-prefix" && i + 1 < argc) g_shotPrefix = argv[++i];
        else if (a == "--post") g_usePost = true;
        else if (a == "--suite" && i + 1 < argc) g_suitePath = argv[++i];
        else if (a == "--fullshot" && i + 1 < argc) g_fullShotPrefix = argv[++i];
    }
    if (!g_suitePath.empty())
        return runSuite(g_suitePath, outPath);
    if (g_script.empty()) g_script = "nihao<SP>";

    if (FAILED(::CoInitialize(nullptr))) { printf("FAIL CoInitialize\n"); return 1; }
    g_edit = ::CreateWindowW(L"EDIT", L"", WS_OVERLAPPEDWINDOW | WS_VISIBLE | ES_LEFT,
                             100, 100, 520, 130, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (!g_edit) { printf("FAIL create edit\n"); return 1; }

    ITfThreadMgr* tm = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITfThreadMgr, (void**)&tm))) { printf("FAIL tm\n"); return 1; }
    TfClientId cid = 0;
    if (FAILED(tm->Activate(&cid))) { printf("FAIL activate\n"); return 1; }
    ITfInputProcessorProfiles* profiles = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITfInputProcessorProfiles, (void**)&profiles))) { printf("FAIL profiles\n"); return 1; }
    if (FAILED(profiles->ActivateLanguageProfile(kTipClsid, 0x0804, kProfileGuid))) { printf("FAIL profile\n"); return 1; }

    ::ShowWindow(g_edit, SW_SHOWNORMAL);
    grabForeground();

    HANDLE th = ::CreateThread(nullptr, 0, [](LPVOID) -> DWORD { runScript(); return 0; }, nullptr, 0, nullptr);
    DWORD start = ::GetTickCount();
    MSG msg;
    for (;;) {
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        if (g_done && ::GetTickCount() - start > 3000) break;
        if (::GetTickCount() - start > 30000) break;
        ::Sleep(10);
    }
    ::WaitForSingleObject(th, 5000);

    wchar_t text[512] = {};
    ::GetWindowTextW(g_edit, text, 512);
    int len = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(len, 0);
    ::WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8.data(), len, nullptr, nullptr);

    FILE* rf = nullptr;
    fopen_s(&rf, outPath.c_str(), "wb");
    if (rf) {
        fprintf(rf, "FOCUS|%d\n", (int)g_focusOk);
        fprintf(rf, "TEXT|%s\n", utf8.c_str());
        fprintf(rf, "SHOTS|");
        for (size_t i = 0; i < g_shots.size(); ++i)
            fprintf(rf, "%s%s:%dx%d", i ? "," : "", g_shots[i].file.c_str(), g_shots[i].w, g_shots[i].h);
        fprintf(rf, "\n");
        fclose(rf);
    }
    printf("DONE %s\n", utf8.c_str());

    profiles->Release();
    tm->Deactivate();
    tm->Release();
    ::CoUninitialize();
    return 0;
}
