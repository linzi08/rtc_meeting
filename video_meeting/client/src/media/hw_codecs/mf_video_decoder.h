// =============================================================================
// Media Foundation H.264 解码器（webrtc::VideoDecoder 实现，m140 签名）
//
// 【硬解优先、软解兜底——优先级在 Configure 里显式实现】
//   尝试顺序：
//     1) MF 硬件解码 MFT（Intel QSV / NVIDIA / AMD，DXVA/D3D11）
//     2) 微软软件 H264 解码 MFT（Windows 自带，稳定兜底）
//   更外层还有第 3 级兜底：组合工厂在协商 VP8/VP9 时直接用 webrtc 内建软解。
//
// 输出：NV12（NV12Buffer）→ 渲染器 NV12 纹理直通路径，无需转 I420。
// =============================================================================
#pragma once

#include <api/video/video_frame.h>
#include <api/video_codecs/video_decoder.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <mutex>
#include <string>
#include <vector>

namespace vm::hw {

class MFH264Decoder : public webrtc::VideoDecoder {
public:
    MFH264Decoder() = default;
    ~MFH264Decoder() override;

    bool    Configure(const Settings& settings) override;
    // 覆盖 3 参版（基类 2 参默认实现会转发到这里）
    int32_t Decode(const webrtc::EncodedImage& input_image, bool missing_frames,
                   int64_t render_time_ms) override;
    int32_t RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback* callback) override;
    int32_t Release() override;
    const char* ImplementationName() const override { return "MF-H264-HW"; }

private:
    // preferHardware=true 先试硬件 MFT，失败自动回退微软软件 MFT（日志明确打印两级结果）
    bool InitMft(bool preferHardware);
    void DestroyMft();

    bool IsAsync() const { return events_ != nullptr; }
    bool WaitForEvent(MediaEventType want, int budgetMs);
    void HandleStreamChange();         // MF_E_TRANSFORM_STREAM_CHANGE → 重协商输出 NV12
    void DeliverNV12(IMFSample* s);    // NV12 样本 → NV12Buffer → Decoded 回调
    bool CurrentFrameSize(int* w, int* h);

    Microsoft::WRL::ComPtr<IMFTransform>           mft_;
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> events_;  // 仅异步 MFT 有
    webrtc::DecodedImageCallback*                  callback_ = nullptr;

    std::string backendName_ = "MF-H264";
    bool        running_     = false;
    std::mutex  mutex_;
    int64_t     lastTsUs_ = 0;
};

}  // namespace vm::hw
