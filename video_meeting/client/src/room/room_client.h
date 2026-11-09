// =============================================================================
// RoomClient —— 客户端核心状态机（设计文档第 2/3 章；libmediasoupclient v3 API）
//
// 职责：
//   1. 信令面：protoo 建连 → getRouterRtpCapabilities → join → createWebRtcTransport
//      → connectWebRtcTransport(库回调触发) → produce(库回调触发)
//   2. 媒体面：采集(摄像头/图案/屏幕/麦克风) → Produce；newConsumer → Consume +
//      resumeConsumer → 绑定渲染窗口
//   3. 会中控件：静音/摄像头开关/屏幕共享/统计/ICE 重启
//   4. 断线重连：指数退避(1s→15s±抖动)，复用 peerId 重入会，
//      服务端在 join 时自动重推 newConsumer → 远端流自动恢复
//   5. SVC：Produce 时经 encodings 参数注入 scalability_mode（无需补丁）
//
// 线程模型（设计文档 3.2）：
//   - 所有流程函数与通知处理都在 taskQueue_ 单线程串行执行；
//   - mediasoupclient 的 OnConnect/OnProduce 发生在 Produce()/Consume() 调用栈
//     内（即 taskQueue_ 线程），其中发同步 protoo 请求不会与 WS 线程互锁；
//   - 会中命令（来自主线程）一律 Post 到 taskQueue_。
// =============================================================================
#pragma once

#include <mediasoupclient.hpp>
#include <json.hpp>

#include <atomic>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../media/audio_capture.h"
#include "../media/camera_capturer.h"
#include "../media/d3d11_renderer.h"
#include "../media/media_engine.h"
#include "../media/pattern_source.h"
#include "../media/screen_capturer.h"
#include "../protoo/protoo_client.h"
#include "../util/task_queue.h"

namespace vm {

class RoomClient : public mediasoupclient::SendTransport::Listener,
                   public mediasoupclient::RecvTransport::Listener,
                   public mediasoupclient::Producer::Listener,
                   public mediasoupclient::Consumer::Listener {
public:
    struct Options {
        std::string url         = "wss://127.0.0.1:4443";  // mediasoup-demo 服务地址
        std::string roomId      = "demo";
        std::string displayName = "native";
        bool        insecure    = true;   // 自签名证书跳过校验（mediasoup-demo 本地部署默认开）
        int         cameraIndex = 0;
        bool        usePattern  = false;  // --pattern / 无摄像头兜底
        bool        screenShare = false;  // 启动即共享屏幕
        int         screenMonitor = 0;
        int         screenFps    = 10;
        bool        noAudio = false, noVideo = false;
        bool        noHw    = false;      // 禁用硬编解码注入（走内建软编）
        std::string svc;  
        std::string origin;                // "L1T3" / "L3T3_KEY" / ""(关闭)
        bool        forceTcp = false;
        int         reconnectMax = 30;    // 最大重连次数
        bool        allowTearing = false; // 渲染跳过 vsync
        std::vector<webrtc::PeerConnectionInterface::IceServer> iceServers;  // --stun/--turn
    };

    explicit RoomClient(const Options& opts);
    ~RoomClient() override;

    bool Start();  // 启动（异步入会，进度看日志）
    void Stop();   // 同步退出（逆序释放全部资源）

    // —— 会中命令（线程安全；投递到任务线程串行执行）——
    void ToggleMic();          // m
    void ToggleCamera();       // v
    void ToggleScreenShare();  // c
    void RestartIceManually(); // r
    void PrintStats();         // s / i

private:
    // ================== 入会 / 重连流程（taskQueue_ 线程） ==================
    void ConnectSignaling();   // 建连（含 peerId 生成）；成功 → OnSignalConnected
    void OnSignalConnected();  // caps → join → transports → publish
    void JoinRoom();
    void CreateTransports();
    void PublishLocal();
    void PublishScreenInternal();
    void CloseScreenInternal();
    void EnterReconnecting();  // 断开 → 退避重连（复用 peerId）
    void TeardownMedia();      // 释放 consumers/producers/transports（保留采集与轨道）
    void RestartIceAll();

    // ================== 通知处理（taskQueue_ 线程） ==================
    void OnNotification(const std::string& method, const nlohmann::json& data);
    void HandleNewConsumer(const nlohmann::json& d);
    void HandleConsumerClosed(const std::string& consumerId);
    void CloseAllConsumersOfPeer(const std::string& peerId);

    // ================== mediasoupclient 回调（Produce/Consume 栈内同步执行） ==================
    // 每个 transport 首次 Produce/Consume 时触发一次 → connectWebRtcTransport
    std::future<void> OnConnect(mediasoupclient::Transport*   transport,
                                const nlohmann::json&         dtlsParameters) override;
    // ICE/DTLS 状态变化（Transport::Listener 纯虚，必须实现）
    void OnConnectionStateChange(mediasoupclient::Transport* transport,
                                 const std::string&          connectionState) override;
    // 每次 Produce → produce 请求（返回 producerId）
    std::future<std::string> OnProduce(mediasoupclient::SendTransport* transport,
                                       const std::string& kind, nlohmann::json rtpParameters,
                                       const nlohmann::json& appData) override;
    // 不使用数据通道：直接拒绝
    std::future<std::string> OnProduceData(mediasoupclient::SendTransport* transport,
                                           const nlohmann::json& sctpStreamParameters,
                                           const std::string&    label,
                                           const std::string&    protocol,
                                           const nlohmann::json& appData) override;
    // Producer/Consumer 随 transport 关闭的回调（本客户端在 TeardownMedia 主动管理）
    void OnTransportClose(mediasoupclient::Producer* producer) override;
    void OnTransportClose(mediasoupclient::Consumer* consumer) override;

    // ---- 远端流记录 ----
    struct ConsumerEntry {
        mediasoupclient::Consumer*           consumer = nullptr;
        std::shared_ptr<render::VideoWindow> window;   // 音频流为空
        std::string                          peerId;
        std::string                          source;   // webcam / mic / screen
    };

    // ---- 本地视频预览 ----
    std::shared_ptr<render::VideoWindow> localCamWindow_;

    // ================== 成员 ==================
    Options               opts_;
    std::string           peerId_;
    protoo::ProtooClient  protoo_;
    TaskQueue              taskQueue_;

    std::unique_ptr<mediasoupclient::Device> device_;
    mediasoupclient::SendTransport* sendTransport_ = nullptr;
    mediasoupclient::RecvTransport* recvTransport_ = nullptr;

    mediasoupclient::Producer* micProducer_    = nullptr;
    mediasoupclient::Producer* camProducer_    = nullptr;
    mediasoupclient::Producer* screenProducer_ = nullptr;
    bool micMuted_ = false, camPaused_ = false;  // 重连后恢复原状态

    // 轨道（跨重连复用；Producer 关闭不影响轨道）
    webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> micTrack_, camTrack_, screenTrack_;

    std::map<std::string, ConsumerEntry> consumers_;  // consumerId → entry
    std::map<std::string, std::string>   peers_;      // peerId → displayName
    std::mutex                           consumersMutex_;  // PrintStats 从主线程读

    // ---- 采集 ----
    webrtc::scoped_refptr<media::LocalVideoSource> camSource_, screenSource_;
    webrtc::scoped_refptr<media::PcmAudioSource>   audioSource_;
    media::CameraCapturer  camera_;
    media::PatternSource   pattern_;
    media::ScreenCapturer  screen_;
    media::WasapiMicCapture mic_;

    // ---- 状态 ----
    enum class State { Idle, Connecting, Joined, Reconnecting, Closing };
    std::atomic<State> state_{State::Idle};
    int                reconnectAttempts_ = 0;
    int64_t            lastIceRestartMs_  = 0;
    std::string        svcActive_;  // 运行期 SVC 模式（协商失败自动清空=降级单层）
};

}  // namespace vm