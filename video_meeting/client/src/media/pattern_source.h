// =============================================================================
// 合成图案视频源（--pattern / 无摄像头时的兜底）
//  - 640x360@30 I420 彩条 + 毫秒时钟烧录在画面中
//  - 【联调利器】对端窗口里的时钟与本地系统时钟之差 ≈ 端到端延迟，截图即测
//  - 纯 CPU 合成，虚拟机可跑通全链路（信令→编码→SFU→解码→渲染）
// =============================================================================
#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "local_video_source.h"

namespace vm::media {

class PatternSource {
public:
    ~PatternSource() { Stop(); }

    bool Start(webrtc::scoped_refptr<LocalVideoSource> source);
    void Stop();

private:
    void Run();

    std::thread                           thread_;
    std::atomic<bool>                     running_{false};
    webrtc::scoped_refptr<LocalVideoSource> source_;
};

}  // namespace vm::media
