// glm-ime TIP —— 命名管道客户端实现。见 PipeClient.h 设计约束。
#include "PipeClient.h"
#include <Windows.h>
#include "json.hpp"
#include <cctype>
#include <cstdio>

static void tipLog(const char* fmt, ...) {
    // 常开日志：%LOCALAPPDATA%\glm-ime\logs\TipLog-<pid>.log（每宿主进程独立）
    static char cachedDir[MAX_PATH] = {0};
    if (!cachedDir[0]) {
        char* la = nullptr; size_t len = 0;
        if (_dupenv_s(&la, &len, "LOCALAPPDATA") == 0 && la) {
            snprintf(cachedDir, sizeof(cachedDir), "%s\\glm-ime", la);
            free(la);
        } else {
            strcpy_s(cachedDir, "C:\\glm-logs");
        }
        ::CreateDirectoryA(cachedDir, nullptr);
        char logs[MAX_PATH];
        snprintf(logs, sizeof(logs), "%s\\logs", cachedDir);
        ::CreateDirectoryA(logs, nullptr);
        strcpy_s(cachedDir, logs);
    }
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\TipLog-%lu.log", cachedDir, (unsigned long)::GetCurrentProcessId());
    FILE* f = nullptr;
    fopen_s(&f, path, "a");
    if (!f) return;
    va_list args; va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

using json = nlohmann::json;

// dllmain.cpp 定义（全局命名空间）：TIP 自身模块句柄
extern HMODULE g_selfModule;

namespace glm {

static std::string Utf8FromWide(const std::wstring& w) {
    if (w.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring Utf8ToWide(const std::string& u) {
    if (u.empty()) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, u.data(), (int)u.size(), nullptr, 0);
    std::wstring w(n, 0);
    ::MultiByteToWideChar(CP_UTF8, 0, u.data(), (int)u.size(), w.data(), n);
    return w;
}

static std::wstring currentUserName() {
    wchar_t buf[256];
    DWORD len = 256;
    if (!::GetUserNameW(buf, &len))
        return L"default";
    return std::wstring(buf, len - 1);
}

PipeClient& PipeClient::instance() {
    static PipeClient inst;
    return inst;
}

PipeClient::~PipeClient() { close(); }

void PipeClient::close() {
    if (pipe_ != INVALID_HANDLE_VALUE && pipe_ != nullptr) {
        ::CloseHandle(pipe_);
        pipe_ = nullptr;
    }
}

// launcher 自愈：连接失败时尝试拉起（单实例 mutex 保证幂等；10s 冷却防风暴）
static void tryStartLauncher() {
    static DWORD lastSpawn = 0;
    DWORD now = ::GetTickCount();
    if (now - lastSpawn < 10000)
        return;
    lastSpawn = now;
    wchar_t dllPath[MAX_PATH] = {};
    if (!::GetModuleFileNameW(g_selfModule, dllPath, MAX_PATH))
        return;
    // <inst>\x64\glm-ime-tip.dll -> <inst>\glm-launcher.exe
    std::wstring p = dllPath;
    size_t pos = p.find_last_of(L'\\');
    if (pos == std::wstring::npos) return;
    p.resize(pos);                          // x64
    pos = p.find_last_of(L'\\');
    if (pos == std::wstring::npos) return;
    p.resize(pos);                          // <inst>
    p += L"\\glm-launcher.exe";
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (::CreateProcessW(p.c_str(), nullptr, nullptr, nullptr, FALSE,
                         0x00000008 /*DETACHED_PROCESS*/, nullptr, nullptr, &si, &pi)) {
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(pi.hThread);
    }
}

bool PipeClient::ensureConnected() {
    if (pipe_ != nullptr && pipe_ != INVALID_HANDLE_VALUE)
        return true;
    if (pipeName_.empty())
        pipeName_ = L"\\\\.\\pipe\\" + currentUserName() + L"\\glm-ime\\launcher";

    HANDLE h = ::CreateFileW(pipeName_.c_str(), GENERIC_READ | GENERIC_WRITE,
                             0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = ::GetLastError();
        tipLog("CreateFile failed err=%lu pipe=%ls", (unsigned long)err, pipeName_.c_str());
        // 管道实例忙（另一端暂未 Accept）：等 launcher 空出实例
        if (err == ERROR_PIPE_BUSY) {
            if (!::WaitNamedPipeW(pipeName_.c_str(), 200))
                return false;
            h = ::CreateFileW(pipeName_.c_str(), GENERIC_READ | GENERIC_WRITE,
                              0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (h == INVALID_HANDLE_VALUE)
                return false;
        } else {
            tryStartLauncher();   // 管道不存在/权限问题：尝试拉起 launcher 自愈
            return false;
        }
    }
    // 消息以行为界：读侧用 ReadFile 循环找 '\n'，写侧一次写整行。字节模式即可。
    pipe_ = h;
    return true;
}

// OVERLAPPED 带超时的同步化读写
static bool overlappedTransfer(HANDLE h, bool read, void* buf, DWORD size, DWORD* transferred, DWORD timeoutMs) {
    OVERLAPPED ov{};
    ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent)
        return false;
    BOOL ok;
    if (read)
        ok = ::ReadFile(h, buf, size, nullptr, &ov);
    else
        ok = ::WriteFile(h, buf, size, nullptr, &ov);
    if (!ok && ::GetLastError() != ERROR_IO_PENDING) {
        tipLog("ov issue err=%lu rw=%d", (unsigned long)::GetLastError(), (int)read);
        ::CloseHandle(ov.hEvent);
        return false;
    }
    DWORD wait = ::WaitForSingleObject(ov.hEvent, timeoutMs);
    if (wait != WAIT_OBJECT_0) {
        tipLog("ov wait timeout/err=%lu rw=%d", (unsigned long)wait, (int)read);
        ::CancelIo(h);
        ::CloseHandle(ov.hEvent);
        return false;
    }
    ok = ::GetOverlappedResult(h, &ov, transferred, FALSE);
    ::CloseHandle(ov.hEvent);
    return ok != FALSE;
}

bool PipeClient::request(const std::string& jsonLine, std::string& reply) {
    if (!ensureConnected()) { tipLog("connect failed"); return false; }
    std::string line = jsonLine + "\n";
    DWORD written = 0;
    if (!overlappedTransfer(pipe_, false, line.data(), (DWORD)line.size(), &written, 4000) || written != line.size()) {
        tipLog("write failed");
        close();
        return false;
    }
    // 读到 '\n' 为止（行协议；响应行短，64KB 单次足够）
    reply.clear();
    char buf[4096];
    for (;;) {
        DWORD got = 0;
        if (!overlappedTransfer(pipe_, true, buf, sizeof(buf), &got, 4000) || got == 0) {
            tipLog("read failed");
            close();
            return false;
        }
        reply.append(buf, got);
        if (reply.find('\n') != std::string::npos)
            break;
    }
    reply.erase(reply.find('\n'));
    return true;
}

bool PipeClient::init(const std::wstring& userDir) {
    json j;
    j["op"] = "init";
    j["user_dir"] = Utf8FromWide(userDir);
    std::string reply;
    if (!request(j.dump(), reply))
        return false;
    try {
        return json::parse(reply).value("ok", false);
    } catch (...) {
        return false;
    }
}

bool PipeClient::reset() {
    std::string reply;
    if (!request(R"({"op":"reset"})", reply))
        return false;
    return true;
}

bool PipeClient::sendKey(UINT code, WCHAR ch, UINT mods, EngineActions& out) {
    json j;
    j["op"] = "key";
    j["code"] = code;
    j["char"] = ch ? Utf8FromWide(std::wstring(1, ch)) : "";
    j["mods"] = mods;
    std::string reply;
    if (!request(j.dump(), reply))
        return false;
    try {
        json r = json::parse(reply);
        if (!r.value("ok", false))
            return false;
        out.consumed = r.value("consumed", false);
        out.actions.clear();
        for (auto& a : r.value("actions", json::array())) {
            std::string t = a.value("action", "");
            if (t == "composition") {
                EngineActions::Action act{EngineActions::Action::Composition};
                act.text = Utf8ToWide(a.value("text", ""));
                act.cursor = a.value("cursor", 0);
                out.actions.push_back(std::move(act));
            } else if (t == "candidates") {
                EngineActions::Action act{EngineActions::Action::Candidates};
                for (auto& c : a.value("list", json::array()))
                    act.list.push_back(Utf8ToWide(c.get<std::string>()));
                out.actions.push_back(std::move(act));
            } else if (t == "commit") {
                EngineActions::Action act{EngineActions::Action::Commit};
                act.text = Utf8ToWide(a.value("text", ""));
                out.actions.push_back(std::move(act));
            } else if (t == "message") {
                EngineActions::Action act{EngineActions::Action::Message};
                act.text = Utf8ToWide(a.value("text", ""));
                act.seconds = a.value("duration", 1);
                out.actions.push_back(std::move(act));
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace glm
