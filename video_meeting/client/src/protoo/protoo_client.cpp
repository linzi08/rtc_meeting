#include "protoo_client.h"

#include "../util/log.h"

namespace vm::protoo {

static const char* TAG = "protoo";

bool ProtooClient::Connect(const std::string& url, bool insecure, 
    const std::string& origin,std::string* error) {
    if (!origin.empty()) {
        ws_.SetOrigin(origin);
    }
    ws_.SetTextCallback([this](const std::string& t) { HandleMessage(t); });
    ws_.SetCloseCallback([this]() {
        LOGT(TAG, "连接断开");
        if (onClosed_) onClosed_();  // RoomClient：进入重连流程
    });
    return ws_.Connect(url, insecure, error);
}

void ProtooClient::Close() {
    LOGT(TAG, "Close() 被调用");
    ws_.Close();
    LOGT(TAG, "Close: ws_ 已关闭，唤醒 pending 请求...");
    // 唤醒所有还在等响应的调用方（超时兜底之外再快一点）
    std::lock_guard<std::mutex> lock(pendingMutex_);
    for (auto& [id, p] : pending_) {
        try {
            p->promise.set_exception(std::make_exception_ptr(
                ProtooException(false, 0, "connection closed")));
        } catch (...) {
            // set_exception 只能成功一次，忽略
        }
    }
    auto n = pending_.size();
    pending_.clear();
    LOGT(TAG, "Close: 完成 (已清理 %zu 个 pending)", n);
}

json ProtooClient::Request(const std::string& method, const json& data, int timeoutMs) {
    uint32_t id = nextId_.fetch_add(1);

    json msg = {
        {"request", true},
        {"id", id},
        {"method", method},
        {"data", data.is_null() ? json::object() : data},
    };

    auto pending = std::make_shared<Pending>();
    auto future  = pending->promise.get_future();
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pending_[id] = pending;
    }

    if (!ws_.SendText(msg.dump())) {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pending_.erase(id);
        throw ProtooException(false, 0, "send failed (method=" + method + ")");
    }

    // 同步等待响应：调用方在 Task 线程，WS 接收线程负责 fulfill，无死锁
    if (future.wait_for(std::chrono::milliseconds(timeoutMs)) != std::future_status::ready) {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pending_.erase(id);
        throw ProtooException(false, 0, "timeout (method=" + method + ")");
    }
    return future.get();
}

void ProtooClient::Notify(const std::string& method, const json& data) {
    json msg = {
        {"notification", true},
        {"method", method},
        {"data", data.is_null() ? json::object() : data},
    };
    ws_.SendText(msg.dump());
}

// WS 接收线程：解析消息并分发（response → promise；notification → 上层）
void ProtooClient::HandleMessage(const std::string& text) {
    try {
    json msg;
    try {
        msg = json::parse(text);
    } catch (const std::exception&) {
        LOGT(TAG, "非 JSON 消息，忽略: %.60s", text.c_str());
        return;
    }

    // 1) 服务端 → 客户端“请求”：mediasoup-demo 不会发，礼貌回应 error 后忽略
    if (msg.value("request", false)) {
        LOGT(TAG, "收到服务端请求 %s（不支持，已忽略）", msg.value("method", "?").c_str());
        json resp = {{"response", true},
                     {"id", msg.value("id", 0)},
                     {"ok", false},
                     {"errorCode", 400},
                     {"errorReason", "not supported by native client"}};
        ws_.SendText(resp.dump());
        return;
    }

    // 2) 响应：配对请求
    if (msg.value("response", false)) {
        uint32_t id = msg.value("id", 0u);
        std::shared_ptr<Pending> pending;
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            auto it = pending_.find(id);
            if (it == pending_.end()) return;  // 已超时被清理
            pending = it->second;
            pending_.erase(it);
        }
        if (msg.value("ok", false)) {
            pending->promise.set_value(msg.contains("data") ? msg["data"] : json::object());
        } else {
            pending->promise.set_exception(std::make_exception_ptr(ProtooException(
                true, msg.value("errorCode", 0), msg.value("errorReason", "server error"))));
        }
        return;
    }

    // 3) 通知：交给 RoomClient（在 Task 队列串行处理，见 RoomClient 绑定处）
    if (msg.value("notification", false)) {
        std::string method = msg.value("method", std::string());
        json        data   = msg.contains("data") ? msg["data"] : json::object();
        if (onNotification_) onNotification_(method, data);
        return;
    }

    LOGT(TAG, "未知消息类型，忽略");
    } catch (const std::exception& e) {
        LOGT(TAG, "HandleMessage 异常: %s", e.what());
    } catch (...) {
        LOGT(TAG, "HandleMessage 未知异常");
    }
}

}  // namespace vm::protoo