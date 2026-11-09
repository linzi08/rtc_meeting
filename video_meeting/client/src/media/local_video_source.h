// =============================================================================
// 本地视频源（摄像头 / 合成图案 / 屏幕共享 共用）
//
// webrtc m140 轨道模型：
//   VideoTrack 持有 VideoTrackSourceInterface；采集侧把 VideoFrame 推给源，
//   源经 VideoSinkInterface::OnFrame 分发给挂在轨道上的 sink
//   （发送端 = mediasoupclient 内部 PeerConnection 的编码通道，本地预览 = 渲染器）。
//
// 线程模型：Broadcast 在采集线程调用；Add/RemoveSink 在信令线程调用（互斥保护）。
// 引用计数：调用方用 webrtc::RefCountedObject 包装（media_engine/room_client）。
// =============================================================================
#pragma once

#include <api/media_stream_interface.h>
#include <api/video/video_frame.h>
#include <api/video/recordable_encoded_frame.h>
#include <api/video/video_sink_interface.h>
#include <api/video/video_source_interface.h>

#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace vm::media {

class LocalVideoSource : public webrtc::VideoTrackSourceInterface {
public:
    explicit LocalVideoSource(bool screencast) : screencast_(screencast) {}

    // —— VideoSourceInterface<VideoFrame>（轨道/发送端挂 sink；wants 由 webrtc
    //    传入的分辨率/帧率约束，本简单源忽略，仅记录 sink）——
    void AddOrUpdateSink(webrtc::VideoSinkInterface<webrtc::VideoFrame>* sink,
                         const webrtc::VideoSinkWants& /*wants*/) override {
        std::lock_guard<std::mutex> lock(m_);
        sinks_.insert(sink);
    }
    void RemoveSink(webrtc::VideoSinkInterface<webrtc::VideoFrame>* sink) override {
        std::lock_guard<std::mutex> lock(m_);
        sinks_.erase(sink);
    }

    // —— MediaSourceInterface（NotifierInterface 部分本源不派发变更事件）——
    webrtc::MediaSourceInterface::SourceState state() const override {
        return webrtc::MediaSourceInterface::kLive;
    }
    bool remote() const override { return false; }
    void RegisterObserver(webrtc::ObserverInterface*) override {}
    void UnregisterObserver(webrtc::ObserverInterface*) override {}

    // —— VideoTrackSourceInterface ——
    bool is_screencast() const override { return screencast_; }
    // 屏幕共享返回 false 提示编码器关闭去噪；摄像头交默认行为（nullopt）
    std::optional<bool> needs_denoising() const override {
        return screencast_ ? std::optional<bool>(false) : std::nullopt;
    }
    // m140 新增的纯虚：源统计/编码输出（本源不支持，返回默认）
    bool GetStats(Stats*) override { return false; }
    bool SupportsEncodedOutput() const override { return false; }
    void GenerateKeyFrame() override {}
    void AddEncodedSink(webrtc::VideoSinkInterface<webrtc::RecordableEncodedFrame>*) override {}
    void RemoveEncodedSink(webrtc::VideoSinkInterface<webrtc::RecordableEncodedFrame>*) override {}

    // —— 采集侧推帧（采集线程调用）→ 所有 sink 的 OnFrame ——
    void Broadcast(const webrtc::VideoFrame& frame) {
        std::vector<webrtc::VideoSinkInterface<webrtc::VideoFrame>*> copy;
        {
            std::lock_guard<std::mutex> lock(m_);
            copy.assign(sinks_.begin(), sinks_.end());
        }
        for (auto* sink : copy) sink->OnFrame(frame);
    }

private:
    bool screencast_;
    std::mutex m_;
    std::set<webrtc::VideoSinkInterface<webrtc::VideoFrame>*> sinks_;
};

}  // namespace vm::media
