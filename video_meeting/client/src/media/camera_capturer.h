// =============================================================================
// 摄像头采集（VCM / DirectShow，libwebrtc 自带实现）
//  - 枚举设备、按序号选择，640x480@30 起步（webrtc 会按带宽自适应降分辨率）
//  - 帧回调 OnFrame → LocalVideoSource::Broadcast（采集线程）
//  - 同机多开注意：摄像头是独占设备，第二个实例请用 --pattern / --no-video
// =============================================================================
#pragma once

#include <modules/video_capture/video_capture.h>
#include <modules/video_capture/video_capture_factory.h>

#include <string>
#include <vector>

#include "local_video_source.h"

namespace vm::media {

class CameraCapturer : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    // 枚举摄像头名（索引即 --camera 参数取值）
    static std::vector<std::string> EnumDevices();

    // 打开 index 号摄像头并开始采集；source 由调用方提供
    bool Start(int index, webrtc::scoped_refptr<LocalVideoSource> source);
    void Stop();
    bool IsRunning() const { return module_ != nullptr; }

    // VCM 帧回调（采集线程）→ 分发给轨道（m140：VideoSinkInterface::OnFrame）
    void OnFrame(const webrtc::VideoFrame& frame) override {
        if (source_) source_->Broadcast(frame);
    }

private:
    webrtc::scoped_refptr<webrtc::VideoCaptureModule> module_;
    webrtc::scoped_refptr<LocalVideoSource>           source_;
};

}  // namespace vm::media
