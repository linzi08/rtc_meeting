// =============================================================================
// RoomClient —— 实现（入会时序/重连/通知处理，与设计文档 2.3 时序一一对应）
// 全部 mediasoupclient 调用按 v3 API：GetId()/GetKind()/GetTrack()/Produce 六参/Consume 六参
// =============================================================================
#include "room_client.h"

#include <chrono>
#include <cstdlib>
#include <random>

#include "../util/log.h"

namespace vm {

static const char* TAG = "room";

using json = nlohmann::json;

// 毫秒时间戳（重连退避/ICE 冷却判断）
static int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

static std::string RandomPeerId() {
    static std::mt19937_64 rng(std::random_device{}());
    char buf[24];
    snprintf(buf, sizeof(buf), "c-%08x%08x", (uint32_t)(rng() & 0xFFFFFFFFu),
             (uint32_t)(rng() & 0xFFFFFFFFu));
    return buf;
}

// =============================================================================
// 生命周期
// =============================================================================
RoomClient::RoomClient(const Options& opts) : opts_(opts) {}
RoomClient::~RoomClient() { Stop(); }

bool RoomClient::Start() {
    if (!opts_.svc.empty()) {
        // SVC 走 Produce 的 encodings 参数注入（webrtc m140 支持，无需补丁）；
        // 若协商结果是 H264（硬编），webrtc 会拒绝分层 → 自动降级单层
        svcActive_ = opts_.svc;
        LOGT(TAG, "SVC 模式: %s（经 encodings 注入；硬编协商时自动降级单层）", svcActive_.c_str());
    }

    taskQueue_.Start();
    taskQueue_.Post([this]() { ConnectSignaling(); });
    return true;
}

void RoomClient::Stop() {
    LOGT(TAG, "Stop() 被调用，开始退出...");
    state_.store(State::Closing);
    protoo_.Close();
    LOGT(TAG, "Stop: protoo 已关闭");
    TeardownMedia();
    LOGT(TAG, "Stop: 媒体已拆除");
    pattern_.Stop();
    camera_.Stop();
    screen_.Stop();
    mic_.Stop();
    camSource_    = nullptr;
    screenSource_ = nullptr;
    audioSource_  = nullptr;
    taskQueue_.Stop();
    LOGT(TAG, "已退出房间");
}

// =============================================================================
// 信令建连 → 入会链（全部在 taskQueue_ 线程）
// =============================================================================
void RoomClient::ConnectSignaling() {
    if (state_.load() == State::Closing) return;

    if (peerId_.empty()) peerId_ = RandomPeerId();  // 重连复用同一 id
    state_.store(State::Connecting);

    // protoo URL：wss://host:4443/?roomId=xxx&peerId=yyy
    std::string url = opts_.url;
    if (!url.empty() && url.back() != '/' && url.back() != '?') url += '/';
    url += (url.find('?') == std::string::npos) ? "?" : "&";
    url += "roomId=" + opts_.roomId + "&peerId=" + peerId_;

    // 通知/断开回调在 WS 线程触发 → 一律 Post 回任务线程串行处理
    protoo_.SetNotificationHandler([this](const std::string& method, const json& data) {
        taskQueue_.Post([this, method, data]() { OnNotification(method, data); });
    });
    protoo_.SetClosedHandler([this]() {
        taskQueue_.Post([this]() {
            if (state_.load() == State::Joined || state_.load() == State::Connecting)
                EnterReconnecting();
        });
    });

    LOGT(TAG, "连接信令服务器 %s (roomId=%s peerId=%s)...", opts_.url.c_str(), opts_.roomId.c_str(),
         peerId_.c_str());

    std::string err;
    if (!protoo_.Connect(url, opts_.insecure, opts_.origin, &err)) {
        LOGT(TAG, "信令连接失败: %s", err.c_str());
        protoo_.Close();
        EnterReconnecting();
        return;
    }
    // 建连成功 → 完整入会链（join 后服务端会自动把房间内已有流推给本端）
    OnSignalConnected();
}

void RoomClient::OnSignalConnected() {
    try {
        // —— (2) 路由能力 → Device::Load ——
        //    服务端响应 data = {"routerRtpCapabilities": {"codecs":[...], "headerExtensions":[...]}}
        //    Device::Load() 期望的是 {"codecs":[...], "headerExtensions":[...]} 本身
        auto routerCapsResp = protoo_.Request("getRouterRtpCapabilities", {}, 5000);
        auto routerCaps = routerCapsResp["routerRtpCapabilities"];
        device_ = std::make_unique<mediasoupclient::Device>();
        //device_->Load(routerCaps, nullptr, /*preferLocalCodecsOrder=*/false);
        // preferLocalCodecsOrder=true：协商顺序按"本地能力"（我们的编码器工厂）决定。
        // 若为 false（mediasoup 默认），ortc::getExtendedRtpCapabilities 会按服务端
        // router 能力顺序排列（mediasoup-demo 默认 VP8 在前），而 ortc::reduceCodecs
        // 在 codec 为 nullptr 时只取 codecs[0] → 永远协商成 VP8，MF 硬编 H264 永远选不中。
        // 本地顺序：H264(MF硬编) 最前；--svc 时 VP9 最前（见 MFBuiltinEncoderFactory）。
        device_->Load(routerCaps, nullptr, /*preferLocalCodecsOrder=*/true);
        LOGT(TAG, "Device 已加载路由能力 (CanProduce audio=%d video=%d)",
             (int)device_->CanProduce("audio"), (int)device_->CanProduce("video"));
        {
            auto clientCaps = device_->GetRtpCapabilities();
            LOGT(TAG, "[DEBUG] 客户端 codecs 数量=%zu", clientCaps.contains("codecs") ? clientCaps["codecs"].size() : 0);
            if (clientCaps.contains("codecs")) {
                for (auto& c : clientCaps["codecs"])
                    LOGT(TAG, "[DEBUG]   客户端 codec: %s", c.contains("mimeType") ? c["mimeType"].get<std::string>().c_str() : "?");
            }
            LOGT(TAG, "[DEBUG] 服务端 codecs 数量=%zu", routerCaps.contains("codecs") ? routerCaps["codecs"].size() : 0);
            if (routerCaps.contains("codecs")) {
                for (auto& c : routerCaps["codecs"])
                    LOGT(TAG, "[DEBUG]   服务端 codec: %s", c.contains("mimeType") ? c["mimeType"].get<std::string>().c_str() : "?");
            }
        }

        // —— (3) join ——
        JoinRoom();

        // —— (4) 两个 transport ——
        CreateTransports();

        // —— (5) 发布本地流 ——
        PublishLocal();

        reconnectAttempts_ = 0;
        state_.store(State::Joined);
        LOGT(TAG, "=== 已入会 %s（成员 %zu 人）===", opts_.roomId.c_str(), peers_.size() + 1);
    } catch (const std::exception& e) {
        LOGT(TAG, "入会失败: %s", e.what());
        protoo_.Close();
        device_.reset();
        EnterReconnecting();
    } catch (...) {
        LOGT(TAG, "入会失败: 未知异常");
        protoo_.Close();
        device_.reset();
        EnterReconnecting();
    }
}

void RoomClient::JoinRoom() {
    json resp = protoo_.Request(
        "join",
        {
            {"displayName", opts_.displayName},
            {"device", {{"flag", "native"}, {"name", "video-meeting-cpp"}, {"version", "1.0"}}},
            {"rtpCapabilities", device_->GetRtpCapabilities()},
            // v1 不用 DataChannel：sctpCapabilities=null → 服务端不建 DataConsumer
            {"sctpCapabilities", nullptr},
        },
        10000);

    // 已在线成员（他们的流会紧随其后以 newConsumer 通知推来）
    peers_.clear();
    if (resp.contains("peers")) {
        for (auto& p : resp["peers"]) peers_[p["peerId"]] = p.value("displayName", "?");
        if (!peers_.empty()) LOGT(TAG, "房间已有成员: %zu 人", peers_.size());
    }
}

void RoomClient::CreateTransports() {
    // ICE 配置（--stun/--turn 注入 mediasoupclient 的 PeerConnection）
    mediasoupclient::PeerConnection::Options pcOpts;
    pcOpts.config.servers = opts_.iceServers;  // m140 库：Options 字段名是 config

    auto create = [&](bool producing, bool consuming) {
        return protoo_.Request(
            "createWebRtcTransport",
            {{"forceTcp", opts_.forceTcp}, {"producing", producing}, {"consuming", consuming},
             {"appData", {{"direction", producing ? "producer" : "consumer"}}}},
            8000);
    };

    // SendTransport（producing）：麦克风/摄像头/屏幕共用
    auto sr = create(true, false);
    sendTransport_ = device_->CreateSendTransport(this, sr["transportId"].get<std::string>(),
                                                  sr["iceParameters"], sr["iceCandidates"],
                                                  sr["dtlsParameters"], &pcOpts);

    // RecvTransport（consuming）：所有远端流
    auto rr = create(false, true);
    recvTransport_ = device_->CreateRecvTransport(this, rr["transportId"].get<std::string>(),
                                                  rr["iceParameters"], rr["iceCandidates"],
                                                  rr["dtlsParameters"], &pcOpts);

    LOGT(TAG, "transports 已创建 (send=%s recv=%s)", sendTransport_->GetId().c_str(),
         recvTransport_->GetId().c_str());
}

void RoomClient::PublishLocal() {
    LOGT(TAG, "[DEBUG] PublishLocal: noAudio=%d noVideo=%d CanProduceAudio=%d CanProduceVideo=%d",
         (int)opts_.noAudio, (int)opts_.noVideo,
         (int)device_->CanProduce("audio"), (int)device_->CanProduce("video"));
    // —— 麦克风（Opus）——
    if (!opts_.noAudio && device_->CanProduce("audio")) {
        if (!audioSource_) {
            audioSource_ = webrtc::scoped_refptr<media::PcmAudioSource>(
                new webrtc::RefCountedObject<media::PcmAudioSource>());
            mic_.Start(audioSource_.get());  // WASAPI 采集线程 → PCM 源
        }
        micTrack_ = media::MediaEngine::Instance().CreateAudioTrack(audioSource_, "mic0");
        json appData{{"source", "mic"}};
        // Produce(producerListener, track, encodings, codecOptions, codec, appData)
        micProducer_ = sendTransport_->Produce(this, micTrack_.get(), nullptr, nullptr, nullptr,
                                               appData);
        LOGT(TAG, "麦克风已发布 (producer=%s)", micProducer_->GetId().c_str());
    }

    // —— 摄像头 / 合成图案（视频）——
    if (!opts_.noVideo && device_->CanProduce("video")) {
        if (!camSource_) {
            camSource_ = webrtc::scoped_refptr<media::LocalVideoSource>(
                new webrtc::RefCountedObject<media::LocalVideoSource>(false));
            // 先试摄像头；打不开（无设备/被独占）→ 合成图案源兜底，保证链路可验证
            bool camOk = false;
            if (!opts_.usePattern) camOk = camera_.Start(opts_.cameraIndex, camSource_);
            if (!camOk) pattern_.Start(camSource_);
        }
        camTrack_ = media::MediaEngine::Instance().CreateVideoTrack(camSource_, "cam0");
        json appData{{"source", "webcam"}};

        // SVC：经 encodings 参数注入 scalability_mode（m140 RtpEncodingParameters
        // 直接支持；sendHandler 会把它写进 SDP，webrtc 侧分层即生效）
        std::vector<webrtc::RtpEncodingParameters> encodings;
        if (!svcActive_.empty()) {
            webrtc::RtpEncodingParameters enc;
            enc.scalability_mode = svcActive_;  // "L1T3" / "L3T3_KEY"（std::optional<std::string>）
            encodings.push_back(enc);
        }
        // camProducer_ = sendTransport_->Produce(this, camTrack_.get(),
        //                                        encodings.empty() ? nullptr : &encodings, nullptr,
        //                                        nullptr, appData);
        // LOGT(TAG, "摄像头已发布 (producer=%s, svc=%s)", camProducer_->GetId().c_str(),
        //      svcActive_.c_str());
        try {
            camProducer_ = sendTransport_->Produce(this, camTrack_.get(),
                                                   encodings.empty() ? nullptr : &encodings,
                                                   nullptr, nullptr, appData);
        } catch (const std::exception& e) {
            LOGT(TAG, "摄像头发布失败: %s", e.what());
            throw;
        }
        {
            const json& rtp = camProducer_->GetRtpParameters();
            std::string mime = "?";
            if (rtp.contains("codecs") && rtp["codecs"].is_array() && !rtp["codecs"].empty())
                mime = rtp["codecs"][0].value("mimeType", "?");
            LOGT(TAG, "摄像头已发布 (producer=%s, 协商codec=%s, svc=%s)",
                 camProducer_->GetId().c_str(), mime.c_str(), svcActive_.c_str());
        }
        // 本地视频预览窗口
        {
            auto* track = static_cast<webrtc::VideoTrackInterface*>(camTrack_.get());
            LOGT(TAG, "本地视频预览: camTrack=%p videoTrack=%p enabled=%d",
                 (void*)camTrack_.get(), (void*)track, track ? (int)track->enabled() : -1);
            if (track) {
                localCamWindow_ = render::RendererManager::Instance().CreateStream("local/webcam", 640, 360);
                if (localCamWindow_) {
                    track->AddOrUpdateSink(localCamWindow_.get(), webrtc::VideoSinkWants());
                    LOGT(TAG, "本地视频预览窗口已创建并绑定 sink");
                } else {
                    LOGT(TAG, "本地视频预览窗口创建失败 (CreateStream 返回 nullptr)");
                }
            } else {
                LOGT(TAG, "本地视频预览跳过: camTrack 无法转为 VideoTrackInterface");
            }
        }
        // 重连后恢复暂停状态
        if (camPaused_ && camProducer_) {
            camProducer_->Pause();
            protoo_.Request("pauseProducer", {{"producerId", camProducer_->GetId()}});
        }
    }

    // —— 屏幕共享（--screen 启动即开；会中 c 键切换）——
    if (opts_.screenShare) PublishScreenInternal();
}

void RoomClient::PublishScreenInternal() {
    if (screenProducer_ || !sendTransport_) return;
    if (!device_->CanProduce("video")) return;

    if (!screenSource_) {
        screenSource_ = webrtc::scoped_refptr<media::LocalVideoSource>(
            new webrtc::RefCountedObject<media::LocalVideoSource>(true /*screencast*/));
    }
    if (!screen_.IsRunning()) {
        if (!screen_.Start(opts_.screenMonitor, opts_.screenFps, screenSource_)) return;
    }
    screenTrack_ = media::MediaEngine::Instance().CreateVideoTrack(screenSource_, "screen0");
    json appData{{"source", "screen"}};  // mediasoup-demo 浏览器端识别为屏幕磁贴
    screenProducer_ =
        sendTransport_->Produce(this, screenTrack_.get(), nullptr, nullptr, nullptr, appData);
    LOGT(TAG, "屏幕共享已发布 (producer=%s)", screenProducer_->GetId().c_str());
}

void RoomClient::CloseScreenInternal() {
    if (!screenProducer_) return;
    try {
        protoo_.Request("closeProducer", {{"producerId", screenProducer_->GetId()}});
    } catch (const std::exception&) {
    }
    screenProducer_->Close();
    screenProducer_ = nullptr;
    screen_.Stop();
    screenTrack_ = nullptr;
    LOGT(TAG, "屏幕共享已停止");
}

// =============================================================================
// mediasoupclient 回调（Produce/Consume 调用栈内 → taskQueue_ 线程同步执行）
// =============================================================================
std::future<void> RoomClient::OnConnect(mediasoupclient::Transport* transport,
                                        const nlohmann::json&       dtlsParameters) {
    // 每个 transport 首次 Produce/Consume 时触发一次（设计文档 2.7-2）
    std::promise<void> pr;
    auto               fut = pr.get_future();
    try {
        protoo_.Request("connectWebRtcTransport",
                        {{"transportId", transport->GetId()}, {"dtlsParameters", dtlsParameters}},
                        5000);
        pr.set_value();
    } catch (...) {
        try {
            pr.set_exception(std::current_exception());
        } catch (...) {
        }
    }
    return fut;
}

void RoomClient::OnConnectionStateChange(mediasoupclient::Transport* transport,
                                         const std::string&          connectionState) {
    LOGT(TAG, "transport %s 状态变化: %s", transport->GetId().c_str(), connectionState.c_str());
    if (connectionState == "failed") {
        LOGT(TAG, "ICE 连接失败，将自动 restartIce");
        taskQueue_.Post([this]() { RestartIceAll(); });
    } else if (connectionState == "disconnected") {
        LOGT(TAG, "transport %s 断开连接", transport->GetId().c_str());
    } else if (connectionState == "connected") {
        LOGT(TAG, "transport %s 已连接", transport->GetId().c_str());
    }
}

std::future<std::string> RoomClient::OnProduce(mediasoupclient::SendTransport* transport,
                                               const std::string& kind, json rtpParameters,
                                               const json& appData) {
    std::promise<std::string> pr;
    auto                      fut = pr.get_future();
    try {
        // SVC：webrtc 侧分层已由 encodings 写入 SDP；这里补到发往服务器的
        // produce 参数（mediasoup 服务端据此创建 SVC Consumer）
        if (kind == "video" && !svcActive_.empty() && rtpParameters.contains("encodings") &&
            rtpParameters["encodings"].is_array() && !rtpParameters["encodings"].empty()) {
            rtpParameters["encodings"][0]["scalabilityMode"] = svcActive_;
            LOGT(TAG, "SVC 已启用 (%s)", svcActive_.c_str());
        }

        json resp = protoo_.Request(
            "produce",
            {{"transportId", transport->GetId()},
             {"kind", kind},
             {"rtpParameters", rtpParameters},
             {"appData", appData}},
            8000);
        pr.set_value(resp["producerId"].get<std::string>());
    } catch (...) {
        try {
            pr.set_exception(std::current_exception());
        } catch (...) {
        }
    }
    return fut;
}

// 不使用数据通道：直接拒绝（保持接口完整）
std::future<std::string> RoomClient::OnProduceData(mediasoupclient::SendTransport*,
                                                   const nlohmann::json&, const std::string&,
                                                   const std::string&, const nlohmann::json&) {
    std::promise<std::string> pr;
    pr.set_exception(
        std::make_exception_ptr(std::runtime_error("data channels not supported")));
    return pr.get_future();
}

void RoomClient::OnTransportClose(mediasoupclient::Producer*) {
    LOGT(TAG, "producer 随 transport 关闭");
}

void RoomClient::OnTransportClose(mediasoupclient::Consumer*) {
    LOGT(TAG, "consumer 随 transport 关闭");
}

void RoomClient::RestartIceAll() {
    int64_t now = NowMs();
    if (now - lastIceRestartMs_ < 10000) return;  // 冷却
    lastIceRestartMs_ = now;

    mediasoupclient::Transport* iceTransports[] = {sendTransport_, recvTransport_};
    for (auto* t : iceTransports) {
        if (!t) continue;
        try {
            auto resp = protoo_.Request("restartIce", {{"transportId", t->GetId()}}, 5000);
            t->RestartIce(resp["iceParameters"]);
            LOGT(TAG, "transport %s ICE 已重启", t->GetId().c_str());
        } catch (const std::exception& e) {
            LOGT(TAG, "restartIce 失败: %s", e.what());
        }
    }
}

// =============================================================================
// 通知处理（设计文档 2.5 全表；taskQueue_ 线程）
// =============================================================================
void RoomClient::OnNotification(const std::string& method, const json& data) {
    if (method == "newConsumer") {
        HandleNewConsumer(data);
    } else if (method == "consumerClosed") {
        HandleConsumerClosed(data["consumerId"]);
    } else if (method == "peerClosed") {
        std::string peerId = data["peerId"];
        peers_.erase(peerId);
        CloseAllConsumersOfPeer(peerId);
        LOGT(TAG, "成员离开: %s", peerId.c_str());
    } else if (method == "newPeer") {
        std::string peerId = data["peerId"];
        peers_[peerId] = data.value("displayName", "?");
        LOGT(TAG, "成员加入: %s (%s)", peers_[peerId].c_str(), peerId.c_str());
    } else if (method == "consumerPaused" || method == "consumerResumed") {
        // 远端暂停了生产（如对端静音/关摄像头）→ 窗口标题打标记
        std::string id     = data["consumerId"];
        bool        paused = (method == "consumerPaused");
        std::lock_guard<std::mutex> lock(consumersMutex_);
        auto it = consumers_.find(id);
        if (it != consumers_.end() && it->second.window)
            it->second.window->SetSuffix(paused ? " [paused]" : "");
    } else if (method == "producerScore") {
        // 上行质量（10 分制），低分提示弱网
        try {
            auto& score = data["score"];
            if (score.is_array() && !score.empty())
                LOGT(TAG, "producerScore %s = %d",
                     data["producerId"].get<std::string>().c_str(), score[0].value("score", 0));
        } catch (const std::exception&) {
        }
    } else if (method == "activeSpeaker") {
        std::string peerId = data.value("peerId", "");
        std::string name   = peers_.count(peerId) ? peers_[peerId] : peerId;
        LOGT(TAG, "当前发言人: %s", name.c_str());
    } else {
        // downlinkBwe / 数据通道相关等：忽略（向前兼容）
        LOGT(TAG, "通知 %s（忽略）", method.c_str());
    }
}

void RoomClient::HandleNewConsumer(const json& d) {
    try {
        std::string id         = d["consumerId"];
        std::string producerId = d["producerId"];
        std::string kind       = d["kind"];
        std::string peerId     = d["peerId"];
        std::string source     = d.contains("appData") && d["appData"].contains("source")
                                     ? d["appData"]["source"].get<std::string>()
                                     : "";
        if (!recvTransport_) {
            LOGT(TAG, "RecvTransport 未就绪，丢弃 newConsumer");
            return;
        }

        json appData = d.contains("appData") ? d["appData"] : json::object();
        // 本地消费端（触发 RecvTransport 的 OnConnect —— 首次时）
        json rtpParams = d["rtpParameters"];  // Consume 需要 json*（库内部可能改写）
        auto* consumer = recvTransport_->Consume(this, id, producerId, kind, &rtpParams, appData);

        // 服务端 consumer 初始为 paused：必须显式 resume（设计文档 2.7-5）
        protoo_.Request("resumeConsumer", {{"consumerId", id}}, 3000);

        ConsumerEntry e;
        e.consumer = consumer;
        e.peerId   = peerId;
        e.source   = source;

        if (kind == "video") {
            // 视频流 → 绑定渲染窗口（D3D11 零拷贝路径）
            std::string name  = peers_.count(peerId) ? peers_[peerId] : peerId.substr(0, 6);
            std::string title = name + "/" + (source == "screen" ? "screen" : "webcam");
            auto* track = static_cast<webrtc::VideoTrackInterface*>(consumer->GetTrack());
            if (track) {
                e.window = render::RendererManager::Instance().CreateStream(title, 640, 360);
                if (e.window)
                    track->AddOrUpdateSink(e.window.get(), webrtc::VideoSinkWants());
            }
        }
        // 音频流：远端播放由 mediasoupclient 内部 ADM 自动完成，无需处理

        {
            std::lock_guard<std::mutex> lock(consumersMutex_);
            consumers_[id] = e;
        }
        LOGT(TAG, "新远端流: %s/%s (%s, consumer=%s)", peerId.c_str(), source.c_str(), kind.c_str(),
             id.c_str());
    } catch (const std::exception& e) {
        LOGT(TAG, "处理 newConsumer 失败: %s", e.what());
    }
}

void RoomClient::HandleConsumerClosed(const std::string& consumerId) {
    std::lock_guard<std::mutex> lock(consumersMutex_);
    auto it = consumers_.find(consumerId);
    if (it == consumers_.end()) return;
    if (it->second.consumer) it->second.consumer->Close();
    if (it->second.window) it->second.window->Close();
    consumers_.erase(it);
    LOGT(TAG, "远端流关闭: %s", consumerId.c_str());
}

void RoomClient::CloseAllConsumersOfPeer(const std::string& peerId) {
    std::lock_guard<std::mutex> lock(consumersMutex_);
    for (auto it = consumers_.begin(); it != consumers_.end();) {
        if (it->second.peerId == peerId) {
            if (it->second.consumer) it->second.consumer->Close();
            if (it->second.window) it->second.window->Close();
            it = consumers_.erase(it);
        } else {
            ++it;
        }
    }
}

// =============================================================================
// 断线重连（设计文档 3.3 / 7）
// =============================================================================
void RoomClient::EnterReconnecting() {
    if (state_.load() == State::Closing) return;
    state_.store(State::Reconnecting);
    protoo_.Close();
    TeardownMedia();

    int attempt = ++reconnectAttempts_;
    if (attempt > opts_.reconnectMax) {
        LOGT(TAG, "重连超过上限 %d 次，放弃（按 q 退出）", opts_.reconnectMax);
        state_.store(State::Closing);
        return;
    }

    // 指数退避 1s→2s→4s→8s→15s(封顶) + ±500ms 抖动
    int delay = 1000 << std::min(attempt - 1, 4);
    if (delay > 15000) delay = 15000;
    delay += (std::rand() % 1000) - 500;

    LOGT(TAG, "连接断开：第 %d/%d 次重连，%.1fs 后重试（复用 peerId=%s）", attempt,
         opts_.reconnectMax, delay / 1000.0, peerId_.c_str());
    taskQueue_.PostDelayed(delay, [this]() { ConnectSignaling(); });
}

// 释放媒体对象（保留采集线程与轨道：重连后直接复用重新 Produce）
void RoomClient::TeardownMedia() {
    LOGT(TAG, "TeardownMedia: 开始拆除媒体对象...");
    {
        std::lock_guard<std::mutex> lock(consumersMutex_);
        for (auto& [id, e] : consumers_) {
            if (e.consumer) e.consumer->Close();
            if (e.window) e.window->Close();
        }
        consumers_.clear();
    }
    if (screenProducer_) {
        screenProducer_->Close();
        screenProducer_ = nullptr;
        screen_.Stop();
        LOGT(TAG, "TeardownMedia: 屏幕共享已关闭");
    }
    if (camProducer_) {
        camProducer_->Close();
        camProducer_ = nullptr;
        LOGT(TAG, "TeardownMedia: 摄像头 producer 已关闭");
    }
    if (localCamWindow_) {
        localCamWindow_->Close();
        localCamWindow_.reset();
        LOGT(TAG, "TeardownMedia: 本地预览窗口已关闭");
    }
    if (micProducer_) {
        micProducer_->Close();
        micProducer_ = nullptr;
        LOGT(TAG, "TeardownMedia: 麦克风 producer 已关闭");
    }
    if (sendTransport_) {
        sendTransport_->Close();
        sendTransport_ = nullptr;
        LOGT(TAG, "TeardownMedia: sendTransport 已关闭");
    }
    if (recvTransport_) {
        recvTransport_->Close();
        recvTransport_ = nullptr;
        LOGT(TAG, "TeardownMedia: recvTransport 已关闭");
    }
    device_.reset();
    LOGT(TAG, "TeardownMedia: 完成");
}

// =============================================================================
// 会中命令（主线程入口 → 任务线程执行）
// =============================================================================
void RoomClient::ToggleMic() {
    taskQueue_.Post([this]() {
        if (!micProducer_) return;
        micMuted_ = !micMuted_;
        try {
            if (micMuted_) {
                micProducer_->Pause();
                protoo_.Request("pauseProducer", {{"producerId", micProducer_->GetId()}});
            } else {
                micProducer_->Resume();
                protoo_.Request("resumeProducer", {{"producerId", micProducer_->GetId()}});
            }
            LOGT(TAG, "麦克风: %s", micMuted_ ? "静音" : "恢复");
        } catch (const std::exception& e) {
            LOGT(TAG, "麦克风切换失败: %s", e.what());
        }
    });
}

void RoomClient::ToggleCamera() {
    taskQueue_.Post([this]() {
        if (!camProducer_) return;
        camPaused_ = !camPaused_;
        try {
            if (camPaused_) {
                camProducer_->Pause();
                protoo_.Request("pauseProducer", {{"producerId", camProducer_->GetId()}});
            } else {
                camProducer_->Resume();
                protoo_.Request("resumeProducer", {{"producerId", camProducer_->GetId()}});
            }
            LOGT(TAG, "摄像头: %s", camPaused_ ? "暂停" : "恢复");
        } catch (const std::exception& e) {
            LOGT(TAG, "摄像头切换失败: %s", e.what());
        }
    });
}

void RoomClient::ToggleScreenShare() {
    taskQueue_.Post([this]() {
        if (screenProducer_) CloseScreenInternal();
        else PublishScreenInternal();
    });
}

void RoomClient::RestartIceManually() {
    taskQueue_.Post([this]() { RestartIceAll(); });
}

void RoomClient::PrintStats() {
    std::lock_guard<std::mutex> lock(consumersMutex_);
    const char* stateStr = "Unknown";
    switch (state_.load()) {
        case State::Idle:         stateStr = "Idle"; break;
        case State::Connecting:   stateStr = "Connecting"; break;
        case State::Joined:       stateStr = "Joined"; break;
        case State::Reconnecting: stateStr = "Reconnecting"; break;
        case State::Closing:      stateStr = "Closing"; break;
    }
    LOGT(TAG, "=== 状态: %s | 房间 %s | 成员 %zu | 远端流 %zu ===", stateStr,
         opts_.roomId.c_str(), peers_.size() + (state_.load() == State::Joined ? 1 : 0),
         consumers_.size());
    LOGT(TAG, "上行: mic=%s(静音%d) cam=%s(暂停%d) screen=%s  SVC=%s", micProducer_ ? "on" : "off",
         (int)micMuted_, camProducer_ ? "on" : "off", (int)camPaused_,
         screenProducer_ ? "on" : "off", svcActive_.empty() ? "关" : svcActive_.c_str());
    for (auto& [id, e] : consumers_) {
        LOGT(TAG, "  consumer %s: peerId=%s source=%s kind=%s", id.c_str(), e.peerId.c_str(),
             e.source.c_str(), e.consumer ? e.consumer->GetKind().c_str() : "?");
    }
}

}  // namespace vm