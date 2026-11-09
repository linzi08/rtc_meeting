// =============================================================================
// WebSocket 客户端（信令传输层）
//
// 实现选型：IXWebSocket（轻量 C++ WebSocket 库，Windows SChannel TLS 后端）。
//   - ws://  明文直连；wss:// 走 SChannel（Windows 系统原生 TLS，零 OpenSSL 依赖）
//   - mediasoup-demo 本地部署是自签名证书：insecure=true 时跳过证书校验
//   - 握手时发送 Sec-WebSocket-Protocol: protoo（protoo-server 强制要求）
//   - 帧编解码 / 分片重组 / ping-pong keepalive 全部由 IXWebSocket 完成
//   - 回调驱动：收到完整"文本消息"后回调 onText（只做解析+投递，不做业务）
//
// 线程模型：Connect/Close 可在任意线程调用（内部有锁）；onText 在 IXWebSocket 后台线程回调。
// =============================================================================
#pragma once

#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXSocketTLSOptions.h>

#include <atomic>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>

namespace vm::net {

class WsClient {
public:
    using TextCallback  = std::function<void(const std::string&)>;
    using CloseCallback = std::function<void()>;

    WsClient() = default;
    ~WsClient();

    WsClient(const WsClient&) = delete;
    WsClient& operator=(const WsClient&) = delete;

    bool Connect(const std::string& url, bool insecure, std::string* error);
    bool SendText(const std::string& text);
    void Close();

    bool IsConnected() const { return connected_.load(); }

    void SetOrigin(const std::string& origin) { origin_ = origin; }
    void SetTextCallback(TextCallback cb) { onText_ = std::move(cb); }
    void SetCloseCallback(CloseCallback cb) { onClose_ = std::move(cb); }

private:
    void FireClosed();

    ix::WebSocket   ws_;
    std::thread     recvThread_;
    std::mutex      sendMutex_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> userClosed_{false};
    std::atomic<bool> closeFired_{false};
    std::string       origin_;
    TextCallback  onText_;
    CloseCallback onClose_;
};

}  // namespace vm::net