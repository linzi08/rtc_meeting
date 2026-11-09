// =============================================================================
// protoo 客户端协议封装（mediasoup-demo 的信令协议）
//
// protoo 三类消息（JSON 文本帧，走 WsClient）：
//   请求  {"request":true,     "id":<int>,"method":"...","data":{...}}
//   响应  {"response":true,    "id":<int>,"ok":true,"data":{...}}
//         {"response":true,    "id":<int>,"ok":false,"errorCode":n,"errorReason":".."}
//   通知  {"notification":true,"method":"...","data":{...}}
//
// 本类职责：id 自增分配、请求-响应配对（promise map）、超时、通知分发。
// 服务端反向请求（mediasoup-demo 不会发）与通知统一打日志忽略，向前兼容。
//
// 线程模型：Request/Notify 在 Task 队列线程调用；响应在 WS 接收线程 fulfill。
// =============================================================================
#pragma once

#include <json.hpp>

#include <atomic>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

#include "../net/ws_client.h"

namespace vm::protoo {

using json = nlohmann::json;

// protoo 异常：remote=true 表示服务端返回 ok:false；false 表示本地超时/断线
struct ProtooException : public std::runtime_error {
    ProtooException(bool remote_, int code_, const std::string& reason_)
        : std::runtime_error(reason_), remote(remote_), code(code_) {}
    bool remote;
    int  code;
};

class ProtooClient {
public:
    using NotificationHandler = std::function<void(const std::string& method, const json& data)>;

    // 连接（URL 形如 wss://host:4443/?room=xxx&peer=yyy，peerId 由调用方生成）
    bool Connect(const std::string& url, bool insecure, const std::string& origin,std::string* error);
    void Close();

    // 同步请求（在 Task 线程调用）。失败抛 ProtooException。
    json Request(const std::string& method, const json& data = json::object(),
                 int timeoutMs = 3000);

    // 单向通知
    void Notify(const std::string& method, const json& data = json::object());

    void SetNotificationHandler(NotificationHandler h) { onNotification_ = std::move(h); }
    // 连接断开（非用户关闭）→ RoomClient 触发重连
    void SetClosedHandler(std::function<void()> h) { onClosed_ = std::move(h); }

    bool IsConnected() const { return ws_.IsConnected(); }

private:
    void HandleMessage(const std::string& text);  // WS 接收线程调用

    net::WsClient ws_;
    std::atomic<uint32_t> nextId_{1};

    struct Pending {
        std::promise<json> promise;
    };
    std::mutex                                  pendingMutex_;
    std::map<uint32_t, std::shared_ptr<Pending>> pending_;

    NotificationHandler onNotification_;
    std::function<void()> onClosed_;
};

}  // namespace vm::protoo
