#include "pattern_source.h"

#include "../util/log.h"

#include <api/video/i420_buffer.h>
#include <chrono>
#include <cmath>

namespace vm::media {

static const char* TAG = "pattern";

// ---------------------------------------------------------------------------
// 5x7 点阵字库（0-9 与 ':' '.'）—— 烧录毫秒时钟用，避免拉字体库
// ---------------------------------------------------------------------------
static const uint8_t kFont[12][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},  // 0
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},  // 1
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},  // 2
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},  // 3
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},  // 4
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},  // 5
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},  // 6
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},  // 7
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},  // 8
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},  // 9
    {0x00, 0x00, 0x08, 0x00, 0x08, 0x00, 0x00},  // :
    {0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00},  // .
};

static void DrawChar(webrtc::scoped_refptr<webrtc::I420Buffer> buf, char c, int x0, int y0,
                     int scale) {
    int idx = -1;
    if (c >= '0' && c <= '9') idx = c - '0';
    else if (c == ':') idx = 10;
    else if (c == '.') idx = 11;
    if (idx < 0) return;
    const int w = buf->width(), h = buf->height();
    uint8_t* y = buf->MutableDataY();
    for (int r = 0; r < 7; ++r) {
        uint8_t bits = kFont[idx][r];
        for (int col = 0; col < 5; ++col) {
            if (!(bits & (1 << (4 - col)))) continue;
            for (int sy = 0; sy < scale; ++sy)
                for (int sx = 0; sx < scale; ++sx) {
                    int px = x0 + col * scale + sx, py = y0 + r * scale + sy;
                    if (px < 0 || px >= w || py < 0 || py >= h) continue;
                    y[py * buf->StrideY() + px] = 235;  // 白（Y 分量，高亮即可）
                }
        }
    }
}

bool PatternSource::Start(webrtc::scoped_refptr<LocalVideoSource> source) {
    Stop();
    source_ = source;
    running_ = true;
    thread_  = std::thread([this] { Run(); });
    LOGT(TAG, "合成图案源已启动 (640x360@30, 含毫秒时钟可测端到端延迟)");
    return true;
}

void PatternSource::Stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    source_ = nullptr;
}

void PatternSource::Run() {
    const int W = 640, H = 360;
    int frame = 0;
    auto next = std::chrono::steady_clock::now();

    while (running_.load()) {
        // 30fps 固定节拍（对齐系统时钟，避免累计漂移）
        next += std::chrono::milliseconds(33);
        std::this_thread::sleep_until(next);

        auto buf = webrtc::I420Buffer::Create(W, H);

        // —— 彩条背景（8 竖条，亮度渐变 + 缓慢滚动，肉眼可判断“画面在动”）——
        uint8_t* y = buf->MutableDataY();
        uint8_t* u = buf->MutableDataU();
        uint8_t* v = buf->MutableDataV();
        int       shift = (frame / 3) % 80;
        for (int r = 0; r < H; ++r) {
            for (int c = 0; c < W; ++c) {
                int bar = ((c + shift) * 8 / W) % 8;
                y[r * buf->StrideY() + c] = (uint8_t)(40 + bar * 25);
            }
        }
        for (int r = 0; r < H / 2; ++r) {
            for (int c = 0; c < W / 2; ++c) {
                int bar = ((c * 2 + shift) * 8 / W) % 8;
                u[r * buf->StrideU() + c] = (uint8_t)(60 + bar * 8);
                v[r * buf->StrideV() + c] = (uint8_t)(180 - bar * 8);
            }
        }

        // —— 毫秒时钟烧录（对端画面与本地时钟差 ≈ 端到端延迟）——
        auto  nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
        int   secs = (int)(nowMs / 1000), ms = (int)(nowMs % 1000);
        char  text[32];
        snprintf(text, sizeof(text), "%02d:%02d:%02d.%03d", (secs / 3600) % 24, (secs / 60) % 60,
                 secs % 60, ms);
        int x = 40, ty = 60, sc = 4;
        for (char* p = text; *p; ++p) {
            DrawChar(buf, *p, x, ty, sc);
            x += (5 + 1) * sc;
        }
        // 帧号（判断是否卡顿/丢帧）
        char fr[16];
        snprintf(fr, sizeof(fr), "%d", frame);
        x = 40;
        for (char* p = fr; *p; ++p) {
            DrawChar(buf, *p, x, 120, 3);
            x += (5 + 1) * 3;
        }

        auto vf = webrtc::VideoFrame::Builder()
                      .set_video_frame_buffer(buf)
                      .set_timestamp_us(std::chrono::duration_cast<std::chrono::microseconds>(
                                            std::chrono::system_clock::now().time_since_epoch())
                                            .count())
                      .build();
        source_->Broadcast(vf);
        ++frame;
    }
}

}  // namespace vm::media
