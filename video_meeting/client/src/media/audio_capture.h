// =============================================================================
// 麦克风采集与 PCM 音频源
//
// 为什么自实现采集（而不用 ADM）：
//   mediasoupclient 内部自建的 PeerConnectionFactory 持有自己的 ADM（负责远端
//   音频播放）。本地麦克风采样的 PCM 通过"自实现 AudioSourceInterface"直接推
//   给轨道 sink（webrtc 的音频发送路径按 track source 驱动），跨工厂也可靠工作；
//   WASAPI 共享模式允许多客户端同时采集，同机多开不受影响。
//
// 远端音频播放：由 mediasoupclient 内部 ADM 自动完成（库内 AudioState 混音）。
// =============================================================================
#pragma once

#include <api/media_stream_interface.h>

#include <atomic>
#include <mutex>
#include <set>
#include <thread>

namespace vm::media {

// ---------------------------------------------------------------------------
// PCM 音频源：采集线程 PushPcm → 所有挂到轨道上的 sink（编码发送通道）
// 注意 m140：AudioTrackSourceInterface 已移除，基类是 AudioSourceInterface
// ---------------------------------------------------------------------------
class PcmAudioSource : public webrtc::AudioSourceInterface {
public:
    // —— AudioSourceInterface（基类有默认实现，这里覆盖为真实分发）——
    void AddSink(webrtc::AudioTrackSinkInterface* sink) override {
        std::lock_guard<std::mutex> lock(m_);
        sinks_.insert(sink);
    }
    void RemoveSink(webrtc::AudioTrackSinkInterface* sink) override {
        std::lock_guard<std::mutex> lock(m_);
        sinks_.erase(sink);
    }

    // —— MediaSourceInterface + NotifierInterface（纯虚，必须实现）——
    webrtc::MediaSourceInterface::SourceState state() const override {
        return webrtc::MediaSourceInterface::kLive;
    }
    bool remote() const override { return false; }
    void RegisterObserver(webrtc::ObserverInterface*) override {}
    void UnregisterObserver(webrtc::ObserverInterface*) override {}

    // —— 采集线程推 PCM（16bit 有符号整型；固定 48kHz 单声道输出给轨道）——
    void PushPcm(const int16_t* data, size_t frames, int sampleRate, int channels) {
        std::lock_guard<std::mutex> lock(m_);
        for (auto* sink : sinks_) {
            sink->OnData(data, /*bits_per_sample=*/16, sampleRate,
                         /*number_of_channels=*/(size_t)channels, frames);
        }
    }

private:
    std::mutex m_;
    std::set<webrtc::AudioTrackSinkInterface*> sinks_;
};

// ---------------------------------------------------------------------------
// WASAPI 共享模式麦克风采集（默认采集设备，10ms 拉取节奏，输出 16bit/48k/单声道）
// ---------------------------------------------------------------------------
class WasapiMicCapture {
public:
    ~WasapiMicCapture() { Stop(); }

    bool Start(PcmAudioSource* sink);
    void Stop();

private:
    void Run();

    std::thread       thread_;
    std::atomic<bool> running_{false};
    PcmAudioSource*   sink_ = nullptr;
};

}  // namespace vm::media
