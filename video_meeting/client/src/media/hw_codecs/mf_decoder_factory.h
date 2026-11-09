// =============================================================================
// 组合解码器工厂
//
//【优先级在 GetSupportedFormats/CreateVideoDecoder 显式体现】
//   H264：MF 解码（MFH264Decoder 内部两级：硬件 MFT → 微软软解 MFT 兜底）
//   VP8/VP9：webrtc 内建软解（libvpx）
// Windows 无三厂统一的 VP8/VP9 硬解路径，故硬解 = H264（详见设计文档 4.5）。
// =============================================================================
#pragma once

#include <api/environment/environment.h>
#include <api/video_codecs/sdp_video_format.h>
#include <api/video_codecs/video_decoder_factory.h>
#include <api/video_codecs/video_decoder_factory_template.h>
#include <api/video_codecs/video_decoder_factory_template_libvpx_vp8_adapter.h>
#include <api/video_codecs/video_decoder_factory_template_libvpx_vp9_adapter.h>

#include <memory>
#include <vector>

namespace vm::hw {

class MFBuiltinDecoderFactory : public webrtc::VideoDecoderFactory {
public:
    MFBuiltinDecoderFactory() = default;

    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override;
    // m140：工厂创建方法统一为 Create(env, format)
    std::unique_ptr<webrtc::VideoDecoder> Create(const webrtc::Environment& env,
                                                 const webrtc::SdpVideoFormat& format) override;

private:
    // 内建软解工厂（VP8/VP9）：用 header-only 模板工厂，避免依赖
    // webrtc.lib 未导出的 CreateBuiltinVideoDecoderFactory 符号
    std::unique_ptr<webrtc::VideoDecoderFactory> builtin_ =
       // webrtc::CreateBuiltinVideoDecoderFactory();
       std::make_unique<webrtc::VideoDecoderFactoryTemplate<
            webrtc::LibvpxVp8DecoderTemplateAdapter,
            webrtc::LibvpxVp9DecoderTemplateAdapter>>();
};

}  // namespace vm::hw
