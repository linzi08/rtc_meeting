// =============================================================================
// 组合编码器工厂（注入 libmediasoupclient 的最终形态）
//
// 【优先级在代码里显式体现——这是硬编优先/软编兜底的唯一决策点】
//   优先级 1：MF 硬件 H264（Intel QSV / NVIDIA NVENC / AMD VCN 的 MF MFT）
//             —— 仅当本机探测到硬件编码 MFT 时才对外提供 H264 格式
//   优先级 2：webrtc 内建软编 VP8/VP9/OpenH264-H264（libvpx/OpenH264）
//             —— 无硬件或未打补丁时自动走这里，永不断流
//
// SVC 特例：--svc 模式需要 VP9 排在首位（协商成 VP9 才能启用分层），
// 构造时传 preferVp9=true 即把 VP9 提到 H264 之前。
// =============================================================================
#pragma once

#include <api/environment/environment.h>
// #include <api/video_codecs/builtin_video_encoder_factory.h>
#include <api/video_codecs/sdp_video_format.h>
#include <api/video_codecs/video_encoder_factory.h>
#include <api/video_codecs/video_encoder_factory_template.h>
#include <api/video_codecs/video_encoder_factory_template_libvpx_vp8_adapter.h>
#include <api/video_codecs/video_encoder_factory_template_libvpx_vp9_adapter.h>
#include <api/video_codecs/video_encoder_factory_template_open_h264_adapter.h>

#include <mfobjects.h>
#include <wrl/client.h>

#include <memory>
#include <string>
#include <vector>

namespace vm::hw {

// 探测是否存在硬件 H264 编码 MFT；vendor 返回如 "NVIDIA H.264 Encoder MFT"
bool FindHardwareEncoderActivate(Microsoft::WRL::ComPtr<IMFActivate>* out,
                                 std::string* vendorName = nullptr);
bool HasHardwareH264Encoder(std::string* vendor = nullptr);

// webrtc 协商用 H264 参数（与 mediasoup-demo 路由能力一致：CB/基线，非交错）
webrtc::SdpVideoFormat H264SdpFormat();

class MFBuiltinEncoderFactory : public webrtc::VideoEncoderFactory {
public:
    explicit MFBuiltinEncoderFactory(bool preferVp9ForSvc = false);

    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override;
    // m140：工厂创建方法统一为 Create(env, format)
    std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment& env,
                                                 const webrtc::SdpVideoFormat& format) override;

private:
    bool preferVp9_ = false;
    // 内建软编工厂（VP8/VP9/OpenH264）——兜底路径
    // std::unique_ptr<webrtc::VideoEncoderFactory> builtin_ = nullptr;
        // webrtc::CreateBuiltinVideoEncoderFactory();
    // 内建软编（VP8/VP9/OpenH264）：header-only 模板工厂，避免依赖
    // webrtc.lib 未导出的 CreateBuiltinVideoEncoderFactory 符号。
    // 禁止置空：GetSupportedFormats/Create 会直接调用它，置空即 0x0 崩溃。
    std::unique_ptr<webrtc::VideoEncoderFactory> builtin_ =
        std::make_unique<webrtc::VideoEncoderFactoryTemplate<
            webrtc::LibvpxVp8EncoderTemplateAdapter,
            webrtc::LibvpxVp9EncoderTemplateAdapter,
            webrtc::OpenH264EncoderTemplateAdapter>>();
};

}  // namespace vm::hw
