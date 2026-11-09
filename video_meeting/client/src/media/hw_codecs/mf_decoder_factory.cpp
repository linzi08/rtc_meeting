#include "mf_decoder_factory.h"

#include "../../util/log.h"
#include "mf_encoder_factory.h"  // H264SdpFormat
#include "mf_video_decoder.h"

namespace vm::hw {

static const char* TAG = "codec";

std::vector<webrtc::SdpVideoFormat> MFBuiltinDecoderFactory::GetSupportedFormats() const {
    // H264（MF 硬解优先/微软软解兜底，见 MFH264Decoder::Configure）+ 内建 VP8/VP9
    // 注：微软软解 MFT 在所有 Windows 上都存在，因此 H264 解码格式无条件提供
    std::vector<webrtc::SdpVideoFormat> formats;
    formats.push_back(H264SdpFormat());
    for (auto& f : builtin_->GetSupportedFormats()) formats.push_back(f);
    LOGT(TAG, "[解码] 优先级: H264-MF(硬件→微软软解两级) > VP8/VP9 内建软解兜底");
    return formats;
}

std::unique_ptr<webrtc::VideoDecoder> MFBuiltinDecoderFactory::Create(
    const webrtc::Environment& env, const webrtc::SdpVideoFormat& format) {
    if (format.name == "H264") {
        // MF 路径：内部"硬件 MFT → 微软软件 MFT"两级兜底
        return std::make_unique<MFH264Decoder>();
    }
    // VP8/VP9 内建软解
    return builtin_->Create(env, format);
}

}  // namespace vm::hw
