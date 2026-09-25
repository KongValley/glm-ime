// glm-ime TIP —— DLL 入口与 COM 注册。
// 模式沿用 PIME：ImeModule 承担 IClassFactory / DllRegisterServer / 语言档案注册。
#include "GlmTextService.h"
#include "ImeModule.h"
#include "DisplayAttributeInfo.h"
#include <Windows.h>
#include <cstdlib>

using namespace Ime;

// 裸 Win32 日志（DllMain 装载锁内安全：无 STL/异常/内存分配的容错路径）
static void dllBootLog(const char* msg) {
    wchar_t dir[MAX_PATH] = {};
    DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    if (n == 0) return;
    wcscat_s(dir, L"\\glm-ime\\logs");
    ::CreateDirectoryW(dir, nullptr);
    wchar_t parent[MAX_PATH] = {};
    n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", parent, MAX_PATH);
    if (n) { wcscat_s(parent, L"\\glm-ime"); ::CreateDirectoryW(parent, nullptr); ::CreateDirectoryW(dir, nullptr); }
    wchar_t path[MAX_PATH] = {};
    swprintf_s(path, L"%s\\Boot-%lu.log", dir, (unsigned long)::GetCurrentProcessId());
    HANDLE h = ::CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    ::WriteFile(h, msg, (DWORD)strlen(msg), &written, nullptr);
    ::WriteFile(h, "\r\n", 2, &written, nullptr);
    ::CloseHandle(h);
}

// {7E4A6B1D-93C2-4E57-9A51-3B8F2D6C0A44} —— glm-ime 独立 CLSID
static const CLSID kTextServiceClsid =
    { 0x7e4a6b1d, 0x93c2, 0x4e57, { 0x9a, 0x51, 0x3b, 0x8f, 0x2d, 0x6c, 0x0a, 0x44 } };

// 语言档案 GUID（ime.json 同步）
// {20BC6F6C-4F74-4600-93DB-8FC81A17C808} 不复用（那是 Vibe/PIME 的），生成新的：
// {5A9E2C4B-1D6F-4A83-B7E2-9C4D8F1A6E3B}
static const GUID kLangProfileGuid =
    { 0x5a9e2c4b, 0x1d6f, 0x4a83, { 0xb7, 0xe2, 0x9c, 0x4d, 0x8f, 0x1a, 0x6e, 0x3b } };

ImeModule* g_imeModule = nullptr;
HMODULE g_selfModule = nullptr; // 供日志打印自身 DLL 路径

class GlmImeModule : public ImeModule {
public:
    GlmImeModule(HMODULE module) :
        ImeModule(module, kTextServiceClsid, createDisplayAttrs()) {
    }

    TextService* createTextService() override {
        return new glm::GlmTextService(this);
    }

private:
    static std::vector<ComPtr<DisplayAttributeInfo>> createDisplayAttrs() {
        // 组词串下划线点式样式（GUID 借用 SampleIME 的 input attribute GUID）
        static const GUID kInputAttribGuid =
            { 0x53bcc437, 0xed0e, 0x4d7d, { 0x9b, 0x46, 0x57, 0xca, 0xb8, 0xa9, 0xf2, 0x55 } };
        auto attrib = ComPtr<DisplayAttributeInfo>(new DisplayAttributeInfo(kInputAttribGuid));
        attrib->setLineStyle(TF_LS_DOT);
        return { std::move(attrib) };
    }
};

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        dllBootLog("ATTACH begin");
        g_selfModule = module;
        g_imeModule = new GlmImeModule(module);
        dllBootLog("ATTACH done (module constructed)");
        break;
    case DLL_PROCESS_DETACH:
        dllBootLog("DETACH");
        if (reserved == nullptr && g_imeModule) {
            g_imeModule->Release(); // 进程显式卸载时释放
            g_imeModule = nullptr;
        }
        break;
    }
    return TRUE;
}

namespace {
Ime::LangProfileInfo langProfile() {
    Ime::LangProfileInfo info;
    info.name = L"glm-ime";
    info.profileGuid = kLangProfileGuid;
    info.locale = L"zh-Hans-CN";
    info.fallbackLocale = L"zh-CN";
    info.iconFile = L"";
    info.iconIndex = 0;
    return info;
}
} // namespace

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (!g_imeModule)
        return E_OUTOFMEMORY;
    if (rclsid != kTextServiceClsid)
        return CLASS_E_CLASSNOTAVAILABLE;
    return g_imeModule->getClassObject(rclsid, riid, ppv);
}

STDAPI DllCanUnloadNow(void) {
    if (!g_imeModule)
        return S_OK;
    return g_imeModule->canUnloadNow();
}

STDAPI DllRegisterServer(void) {
    if (!g_imeModule)
        return E_OUTOFMEMORY;
    Ime::LangProfileInfo profile = langProfile();
    return g_imeModule->registerServer(const_cast<LPWSTR>(L"glm-ime"), &profile, 1);
}

STDAPI DllUnregisterServer(void) {
    if (!g_imeModule)
        return E_OUTOFMEMORY;
    return g_imeModule->unregisterServer();
}
