#include "ws_client.h"

#include "../util/log.h"

namespace vm::net {

WsClient::~WsClient() {
    Close();
}

bool WsClient::Connect(const std::string& url, bool insecure, std::string* error) {
    ix::initNetSystem();

    LOGT("ws", "Connect URL: %s", url.c_str());

    userClosed_  = false;
    closeFired_  = false;

    // 1) 配置 URL
    LOGT("ws", "[DEBUG] 1. setUrl");
    ws_.setUrl(url);

    // 2) 握手时发送 Sec-WebSocket-Protocol: protoo（protoo-server 强制要求）
    // LOGT("ws", "[DEBUG] 2. setExtraHeaders (Sec-WebSocket-Protocol: protoo)");
    // ws_.setExtraHeaders({
    //     {"Sec-WebSocket-Protocol", "protoo"}
    // });
    //    服务端 mediasoup-demo 校验 Origin 白名单：显式提供以覆盖 IXWebSocket 自动值
    ix::WebSocketHttpHeaders headers;
    headers["Sec-WebSocket-Protocol"] = "protoo";
    if (!origin_.empty()) headers["Origin"] = origin_;
    std::string hdr = "[DEBUG] 2. setExtraHeaders (protoo"
                   + std::string(origin_.empty() ? ", Origin=auto" : ", Origin=" + origin_) + ")";
    LOGT("ws", "%s", hdr.c_str());
    ws_.setExtraHeaders(headers);

    // 3) 自签名证书：跳过校验（--insecure 默认开启；正式部署请配真实证书并关掉）
    LOGT("ws", "[DEBUG] 3. setTLSOptions (insecure=%d)", (int)insecure);
    if (insecure) {
        ix::SocketTLSOptions tlsOpts;
        tlsOpts.certFile = "";
        tlsOpts.caFile  = "NONE";                    // 禁用 peer 证书校验
        tlsOpts.disable_hostname_validation = true;   // 跳过主机名校验
        ws_.setTLSOptions(tlsOpts);
    }

    // 4) 15s 心跳（ping/pong，防止中间设备掐空闲连接）
    LOGT("ws", "[DEBUG] 4. setPingInterval=15");
    ws_.setPingInterval(15);

    // 5) 设置消息回调（对应原 RecvLoop + 分片重组 + 关闭帧处理）
    LOGT("ws", "[DEBUG] 5. setOnMessageCallback");
    ws_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        try {
        switch (msg->type) {
            case ix::WebSocketMessageType::Open:
                LOGT("ws", "已连接");
                connected_ = true;
                break;

            case ix::WebSocketMessageType::Message:
                LOGT("ws", "<<< %s", msg->str.c_str());
                if (onText_) onText_(msg->str);
                break;

            case ix::WebSocketMessageType::Close:
                LOGT("ws", "连接关闭: code=%u reason=%s",
                     msg->closeInfo.code, msg->closeInfo.reason.c_str());
                connected_ = false;
                FireClosed();
                break;

            case ix::WebSocketMessageType::Error:
                LOGT("ws", "连接错误: %s", msg->errorInfo.reason.c_str());
                connected_ = false;
                FireClosed();
                break;

            default:
                break;
        }
        } catch (const std::exception& e) {
            LOGT("ws", "WS 回调异常: %s", e.what());
        } catch (...) {
            LOGT("ws", "WS 回调未知异常");
        }
    });

    // 6) 同步握手（IXWebSocket 阻塞模式：先 connect 再 run）
    //    connect() 在调用线程做 TLS+WebSocket 握手；成功后起后台线程 run() 做 poll/dispatch
    //    IXWebSocket 内部超时可能失效（OpenSSL SSL_connect 在非阻塞 socket 上可能卡死），
    //    因此用 std::async + wait_for 加一层外部超时保护（6秒）
    LOGT("ws", "[DEBUG] 6. ws_.connect(5) with external timeout 6s ...");
    auto future = std::async(std::launch::async, [this]() -> ix::WebSocketInitResult {
        return ws_.connect(5);
    });
    auto status = future.wait_for(std::chrono::seconds(6));
    if (status == std::future_status::timeout) {
        LOGT("ws", "[DEBUG] 6. ws_.connect() 外部超时！调用 ws_.stop() 强制终止");
        ws_.stop();
        future.wait();
        LOGT("ws", "连接失败: 握手超时 (6s)，IXWebSocket connect() 未返回");
        if (error) *error = "连接失败: 握手超时 (6s)";
        return false;
    }
    auto result = future.get();
    LOGT("ws", "[DEBUG] 6. ws_.connect() returned: success=%d errorStr=%s",
         (int)result.success, result.errorStr.c_str());
    if (!result.success) {
        LOGT("ws", "连接失败: %s", result.errorStr.c_str());
        if (error) *error = "连接失败: " + result.errorStr;
        return false;
    }

    // 7) 握手成功 → 启动接收线程（poll/dispatch 循环，持续读取服务端消息）
    LOGT("ws", "[DEBUG] 7. 启动 recvThread (ws_.run)");
    recvThread_ = std::thread([this]() {
        LOGT("ws", "[DEBUG] recvThread 已启动, 进入 ws_.run()");
        ws_.run();
        LOGT("ws", "[DEBUG] ws_.run() 已返回, recvThread 退出");
    });

    return true;
}

bool WsClient::SendText(const std::string& text) {
    if (!connected_.load()) {
        LOGT("ws", "[DEBUG] SendText 跳过: 未连接");
        return false;
    }
    LOGT("ws", ">>> %s", text.c_str());
    std::lock_guard<std::mutex> lock(sendMutex_);
    auto info = ws_.send(text);
    if (!info.success) {
        LOGT("ws", "发送失败 (compressionError=%d)", (int)info.compressionError);
    }
    return info.success;
}

void WsClient::Close() {
    LOGT("ws", "Close() 被调用");
    userClosed_ = true;

    ws_.close();
    LOGT("ws", "Close: ws_.close() 完成");

    ws_.stop();
    LOGT("ws", "Close: ws_.stop() 完成");

    if (recvThread_.joinable()) {
        recvThread_.join();
        LOGT("ws", "Close: recvThread_ 已 join");
    }

    connected_ = false;
    LOGT("ws", "Close: 完成");
}

void WsClient::FireClosed() {
    // 非用户主动关闭时通知上层（RoomClient 由此触发重连）；只触发一次
    // 与原逻辑完全一致：compare_exchange_strong 保证只触发一次，
    // !userClosed_ 保证只在非用户主动关闭时触发
    bool expect = false;
    if (closeFired_.compare_exchange_strong(expect, true)) {
        if (!userClosed_.load() && onClose_) onClose_();
    }
}

}  // namespace vm::net