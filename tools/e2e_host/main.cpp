// glm-ime TSF 端到端测试宿主。
// 关键：窗口线程必须泵消息（否则 SendInput 的输入永远不被分发）。
// 主线程：窗口 + TSF 激活 + 消息泵；工作线程：SendInput 打字。
#include <windows.h>
#include <msctf.h>
#include <objbase.h>
#include <stdio.h>
#include <string>
#include <atomic>
#include <thread>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "uuid.lib")

static const CLSID kTipClsid =
    { 0x7e4a6b1d, 0x93c2, 0x4e57, { 0x9a, 0x51, 0x3b, 0x8f, 0x2d, 0x6c, 0x0a, 0x44 } };
static const GUID kProfileGuid =
    { 0x5a9e2c4b, 0x1d6f, 0x4a83, { 0xb7, 0xe2, 0x9c, 0x4d, 0x8f, 0x1a, 0x6e, 0x3b } };

static HWND g_edit = nullptr;
static std::atomic<bool> g_done{false};

static void typeKey(WORD vk) {
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = vk;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = vk;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    ::SendInput(2, inputs, sizeof(INPUT));
}

static void typeString(const char* s) {
    for (const char* p = s; *p; ++p) {
        WORD vk = (WORD)(toupper((unsigned char)*p));
        typeKey(vk);
        ::Sleep(40);
    }
}

static DWORD WINAPI typingThread(LPVOID) {
    ::Sleep(1000); // 等主线程完成 SetFocus/TSF 激活
    typeString("nihao");
    ::Sleep(300);
    typeKey(VK_SPACE);
    ::Sleep(700);
    g_done = true;
    return 0;
}

int main() {
    if (FAILED(::CoInitialize(nullptr))) { printf("FAIL CoInitialize\n"); return 1; }

    g_edit = ::CreateWindowW(L"EDIT", L"", WS_OVERLAPPEDWINDOW | WS_VISIBLE | ES_LEFT,
                             100, 100, 500, 120, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (!g_edit) { printf("FAIL create edit\n"); return 1; }

    ITfThreadMgr* tm = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITfThreadMgr, (void**)&tm)) || !tm) {
        printf("FAIL TF_ThreadMgr\n"); return 1;
    }
    TfClientId cid = 0;
    if (FAILED(tm->Activate(&cid))) { printf("FAIL ThreadMgr Activate\n"); return 1; }

    ITfInputProcessorProfiles* profiles = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITfInputProcessorProfiles, (void**)&profiles)) || !profiles) {
        printf("FAIL InputProcessorProfiles\n"); return 1;
    }
    HRESULT hr = profiles->ActivateLanguageProfile(kTipClsid, 0x0804, kProfileGuid);
    printf("ActivateLanguageProfile: hr=0x%08X\n", (unsigned)hr);
    if (FAILED(hr)) return 1;

    // 前台焦点（AttachThreadInput 技巧）
    ::ShowWindow(g_edit, SW_SHOWNORMAL);
    DWORD myThread = ::GetCurrentThreadId();
    for (int i = 0; i < 5 && ::GetForegroundWindow() != g_edit; ++i) {
        DWORD fgThread = ::GetWindowThreadProcessId(::GetForegroundWindow(), nullptr);
        ::AttachThreadInput(myThread, fgThread, TRUE);
        ::BringWindowToTop(g_edit);
        ::SetForegroundWindow(g_edit);
        ::SetFocus(g_edit);
        ::AttachThreadInput(myThread, fgThread, FALSE);
        ::Sleep(300);
    }
    printf("foreground=%s\n", ::GetForegroundWindow() == g_edit ? "edit" : "OTHER");

    // 打字线程
    HANDLE th = ::CreateThread(nullptr, 0, typingThread, nullptr, 0, nullptr);

    // 消息泵（必须！否则输入永远不分发）
    DWORD start = ::GetTickCount();
    MSG msg;
    for (;;) {
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        if (g_done && ::GetTickCount() - start > 4000) break;
        if (::GetTickCount() - start > 20000) break;
        ::Sleep(10);
    }
    ::WaitForSingleObject(th, 5000);

    wchar_t text[256] = {};
    ::GetWindowTextW(g_edit, text, 256);
    int len = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(len, 0);
    ::WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8.data(), len, nullptr, nullptr);
    printf("EDIT TEXT: %s\n", utf8.c_str());
    bool ok = (text == std::wstring(L"你好"));
    printf(ok ? "E2E PASS\n" : "E2E FAIL\n");
    {
        FILE* f = nullptr;
char resultPath[MAX_PATH] = {};
        ::GetEnvironmentVariableA("TEMP", resultPath, MAX_PATH);
        strncat_s(resultPath, "\\e2e_result.txt", _TRUNCATE);
        fopen_s(&f, resultPath, "w");
        if (f) {
            fprintf(f, "%s|%s\n", ok ? "PASS" : "FAIL", utf8.c_str());
            fclose(f);
        }
    }

    profiles->Release();
    tm->Deactivate();
    tm->Release();
    ::CoUninitialize();
    return ok ? 0 : 1;
}
