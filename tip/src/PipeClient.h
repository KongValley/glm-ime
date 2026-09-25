// glm-ime TIP —— 命名管道客户端（协议 engine.v0，见 proto/engine.v0.md）。
// 设计约束（教训 #5）：TIP 运行在宿主应用进程内 —— 任何阻塞/崩溃都会拖垮宿主。
// 因此：OVERLAPPED + 事件 + 5s 超时；断线不抛异常，返回错误由上层清空状态。

#pragma once
#include <Windows.h>
#include <string>
#include <vector>
#include <optional>

namespace glm {

struct EngineActions {
    bool consumed = false;
    // 动作序列（有序）：composition / candidates / commit / message
    struct Action {
        enum Type { Composition, Candidates, Commit, Message } type;
        std::wstring text;              // composition/commit/message
        int cursor = 0;                 // composition
        std::vector<std::wstring> list; // candidates
        int seconds = 0;                // message
    };
    std::vector<Action> actions;
};

class PipeClient {
public:
    // user 名进入管道路径（与 launcher 一致：\\.\pipe\<user>\glm-ime\launcher）
    static PipeClient& instance();

    // 发送 init（每次 Activate 调一次；会话状态在引擎侧归零）
    bool init(const std::wstring& userDir = L"");
    // 发送按键请求；false = 通信失败（上层应清空状态并不吃键）
    bool sendKey(UINT code, WCHAR ch, UINT mods, EngineActions& out);
    // 请求引擎退出会话状态（可选调用）
    bool reset();

private:
    PipeClient() = default;
    ~PipeClient();
    PipeClient(const PipeClient&) = delete;
    PipeClient& operator=(const PipeClient&) = delete;

    bool ensureConnected();
    bool request(const std::string& jsonLine, std::string& reply);
    void close();

    void* pipe_ = nullptr; // HANDLE
    std::wstring pipeName_;
};

} // namespace glm
