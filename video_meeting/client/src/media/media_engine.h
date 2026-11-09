// =============================================================================
// 媒体引擎：轨道工厂 + 硬编解码注入
//
// 说明（重要）：
//  - mediasoupclient 在其 PeerConnection 构造里调用 webrtc::CreatePeerConnectionFactory
//    自建 PCF（供 Send/RecvTransport 使用），写死内建软编工厂。补丁
//    （patches/README.md）在该调用处把视频工厂参数替换为我们的"MF 硬编 H264 +
//    内建软编"组合工厂——这就是硬编解生效的唯一通道。
//  - 本类自己另建一个 PeerConnectionFactory（CreateModularPeerConnectionFactory +
//    EnableMedia），只用于"创建本地轨道"（VideoTrack / AudioTrack）。视频轨道由
//    源驱动、音频轨道用自实现 PCM 源，因此该工厂的编解码器/ADM 均不参与实际收发。
// =============================================================================
#pragma once

#include <api/media_stream_interface.h>
#include <api/peer_connection_interface.h>
#include <rtc_base/thread.h>

#include <memory>
#include <string>

#include "audio_capture.h"
#include "local_video_source.h"

namespace vm::media {

class MediaEngine {
public:
    static MediaEngine& Instance();

    // 初始化（进程一次）：
    //   enableHwInject  是否注入 MF 硬编解码工厂（需补丁；由 --no-hw / 编译开关决定）
    //   preferVp9ForSvc SVC 模式下 VP9 排到 H264 之前（协商成 VP9 才能开分层）
    bool Init(bool enableHwInject, bool preferVp9ForSvc);
    void Shutdown();

    // 创建本地轨道（m140：CreateVideoTrack(source, label) 源在前）
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> CreateVideoTrack(
        webrtc::scoped_refptr<LocalVideoSource> source, const std::string& id);
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> CreateAudioTrack(
        webrtc::scoped_refptr<PcmAudioSource> source, const std::string& id);

private:
    MediaEngine() = default;

    std::unique_ptr<webrtc::Thread> networkThread_;
    std::unique_ptr<webrtc::Thread> workerThread_;
    std::unique_ptr<webrtc::Thread> signalingThread_;
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> pcf_;
};

}  // namespace vm::media
