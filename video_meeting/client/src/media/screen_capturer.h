// =============================================================================
// 屏幕共享（webrtc::DesktopCapturer，DXGI Duplication 优先 / GDI 兜底）
//  - 默认 10fps、含鼠标光标合成（DesktopAndCursorComposer）
//  - 输出轨道标记 is_screencast=true；produce 时 appData={source:'screen'}，
//    mediasoup-demo 浏览器端原生识别为“屏幕共享”磁贴（服务端零改动）
// =============================================================================
#pragma once

#include <modules/desktop_capture/desktop_and_cursor_composer.h>
#include <modules/desktop_capture/desktop_capture_options.h>
#include <modules/desktop_capture/desktop_capturer.h>

#include <atomic>
#include <thread>

#include "local_video_source.h"

namespace vm::media {

class ScreenCapturer : public webrtc::DesktopCapturer::Callback {
public:
    ~ScreenCapturer() override { Stop(); }

    // monitorIndex: 显示器序号（0 主屏）；fps: 5~30
    bool Start(int monitorIndex, int fps, webrtc::scoped_refptr<LocalVideoSource> source);
    void Stop();
    bool IsRunning() const { return running_.load(); }

    // DesktopCapturer::Callback（采集线程）
    void OnCaptureResult(webrtc::DesktopCapturer::Result result,
                         std::unique_ptr<webrtc::DesktopFrame> frame) override;

private:
    void Run();

    std::unique_ptr<webrtc::DesktopCapturer> capturer_;  // 含光标合成包装
    webrtc::scoped_refptr<LocalVideoSource>  source_;
    std::thread                              thread_;
    std::atomic<bool>                        running_{false};
    int                                      fps_ = 10;
};

}  // namespace vm::media
