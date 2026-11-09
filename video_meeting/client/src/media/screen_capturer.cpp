#include "screen_capturer.h"

#include "../util/log.h"

#include <api/video/i420_buffer.h>
#include <libyuv.h>

#include <chrono>

namespace vm::media {

static const char* TAG = "screen";

bool ScreenCapturer::Start(int monitorIndex, int fps, webrtc::scoped_refptr<LocalVideoSource> source) {
    Stop();
    if (fps < 5) fps = 5;
    if (fps > 30) fps = 30;
    fps_    = fps;
    source_ = source;

    // DXGI Duplication（Win8+，高效率）失败时 CreateScreenCapturer 自动回退 GDI
    auto options = webrtc::DesktopCaptureOptions::CreateDefault();

    // 光标合成包装：画面里叠鼠标指针（远端看到的与自己看到的一致）
    capturer_ = std::make_unique<webrtc::DesktopAndCursorComposer>(
        webrtc::DesktopCapturer::CreateScreenCapturer(options), options);
    if (!capturer_) {
        LOGT(TAG, "创建屏幕采集器失败");
        return false;
    }

    // 选显示器（0 = 主屏；多显示器按序号）。m140：GetSourceList 返回 bool
    webrtc::DesktopCapturer::SourceList sources;
    if (capturer_->GetSourceList(&sources) && !sources.empty()) {
        size_t idx = (size_t)monitorIndex < sources.size() ? monitorIndex : 0;
        capturer_->SelectSource(sources[idx].id);
    }

    running_ = true;
    thread_  = std::thread([this] { Run(); });
    LOGT(TAG, "屏幕共享已启动 (monitor=%d, %dfps, 含光标)", monitorIndex, fps_);
    return true;
}

void ScreenCapturer::Stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    capturer_.reset();
    source_ = nullptr;
}

void ScreenCapturer::Run() {
    auto next = std::chrono::steady_clock::now();
    while (running_.load()) {
        // 固定帧率节拍（对齐系统时钟）
        next += std::chrono::milliseconds(1000 / fps_);
        std::this_thread::sleep_until(next);

        // CaptureFrame 同步回调 OnCaptureResult（同线程）
        capturer_->CaptureFrame();
    }
}

void ScreenCapturer::OnCaptureResult(webrtc::DesktopCapturer::Result result,
                                     std::unique_ptr<webrtc::DesktopFrame> frame) {
    if (result != webrtc::DesktopCapturer::Result::SUCCESS || !frame || !source_) return;

    int w = frame->size().width(), h = frame->size().height();
    if (w <= 0 || h <= 0) return;

    // DesktopFrame 是 BGRA 内存；libyuv 的 "ARGB" 即小端 BGRA 字节序，直接转 I420
    auto buf = webrtc::I420Buffer::Create(w, h);
    libyuv::ARGBToI420(frame->data(), frame->stride(), buf->MutableDataY(), buf->StrideY(),
                       buf->MutableDataU(), buf->StrideU(), buf->MutableDataV(), buf->StrideV(),
                       w, h);

    auto vf = webrtc::VideoFrame::Builder()
                  .set_video_frame_buffer(buf)
                  .set_timestamp_us(std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count())
                  .build();
    source_->Broadcast(vf);
}

}  // namespace vm::media
