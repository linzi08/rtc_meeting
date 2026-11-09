#include "mf_encoder_factory.h"

#include "../../util/log.h"
#include "mf_video_encoder.h"

#include <api/video_codecs/video_codec.h>
#include <api/video_codecs/video_encoder.h>

namespace vm::hw {

static const char* TAG = "codec";

bool HasHardwareH264Encoder(std::string* vendor) {
    Microsoft::WRL::ComPtr<IMFActivate> act;
    return FindHardwareEncoderActivate(&act, vendor);
}

webrtc::SdpVideoFormat H264SdpFormat() {
    webrtc::SdpVideoFormat f("H264");
    f.parameters["level-asymmetry-allowed"] = "1";
    f.parameters["packetization-mode"]      = "1";
    f.parameters["profile-level-id"]        = "42e01f";  // Constrained Baseline
    return f;
}

MFBuiltinEncoderFactory::MFBuiltinEncoderFactory(bool preferVp9ForSvc)
    : preferVp9_(preferVp9ForSvc) {}

// ---------------------------------------------------------------------------
// 能力列表 = 决定 SDP 协商结果与本工厂的"硬/软"走向
// ---------------------------------------------------------------------------
std::vector<webrtc::SdpVideoFormat> MFBuiltinEncoderFactory::GetSupportedFormats() const {
    std::vector<webrtc::SdpVideoFormat> formats;

    std::string vendor;
    bool hasHw = HasHardwareH264Encoder(&vendor);

    // 软编格式（内建：VP8/VP9/OpenH264-H264/AV1）
    std::vector<webrtc::SdpVideoFormat> soft = builtin_->GetSupportedFormats();

    if (hasHw) {
        // —— 硬编优先：H264 排最前（webrtc 按序协商，H264 会中选走硬编）——
        formats.push_back(H264SdpFormat());
        LOGT(TAG, "[编码] 硬件 H264 可用(%s) → 优先级: H264硬编 > 内建软编兜底",
             vendor.c_str());
    } else {
        // —— 兜底：无硬件编码器，完全不提供 MF-H264，协商自动落内建软编 ——
        LOGT(TAG, "[编码] 未检测到硬件 H264 编码器 → 兜底: 内建软编 VP8/VP9/OpenH264");
    }

    if (preferVp9_) {
        // SVC 模式：VP9 必须排首位（协商成 VP9 才能启用 scalabilityMode 分层）
        for (auto& f : soft) {
            if (f.name == "VP9") formats.insert(formats.begin(), f);
        }
    }
    for (auto& f : soft) formats.push_back(f);
    return formats;
}

// 软编代理：仅包装内建编码器，在 InitEncode 时把实际协商的类型/分辨率打出来
// （硬编路径 MFH264Encoder::InitEncode 已有同类日志，故此代理只覆盖软编）
class LogProxyEncoder : public webrtc::VideoEncoder {
public:
    LogProxyEncoder(std::unique_ptr<webrtc::VideoEncoder> inner, std::string name)
        : inner_(std::move(inner)), name_(std::move(name)) {}
 
    int InitEncode(const webrtc::VideoCodec* c, const webrtc::VideoEncoder::Settings& s) override {
        const char* type = "?";
        switch (c->codecType) {
            case webrtc::kVideoCodecVP8:  type = "VP8";  break;
            case webrtc::kVideoCodecVP9:  type = "VP9";  break;
            case webrtc::kVideoCodecH264: type = "H264"; break;
            case webrtc::kVideoCodecAV1:  type = "AV1";  break;
            default: break;
        }
        LOGT(TAG, "[编码] 实际编码器(软编): 类型=%s(name=%s) 分辨率=%dx%d@%dfps",
             type, name_.c_str(), c->width, c->height,
             c->maxFramerate > 0 ? c->maxFramerate : 30);
        return inner_->InitEncode(c, s);
    }
    int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* cb) override {
        return inner_->RegisterEncodeCompleteCallback(cb);
    }
    int32_t Release() override { return inner_->Release(); }
    int32_t Encode(const webrtc::VideoFrame& frame,
                   const std::vector<webrtc::VideoFrameType>* types) override {
        return inner_->Encode(frame, types);
    }
    void SetRates(const RateControlParameters& p) override { inner_->SetRates(p); }
    webrtc::VideoEncoder::EncoderInfo GetEncoderInfo() const override {
        return inner_->GetEncoderInfo();
    }
 
private:
    std::unique_ptr<webrtc::VideoEncoder> inner_;
    std::string name_;
};

std::unique_ptr<webrtc::VideoEncoder> MFBuiltinEncoderFactory::Create(
    const webrtc::Environment& env, const webrtc::SdpVideoFormat& format) {
    // 硬编=只有 H264；软编=VP8/VP9/H264(OpenH264)/AV1
    LOGT(TAG, "[编码] 协商实际选中的编码器: %s", format.name.c_str());
    if (format.name == "H264") {
        std::string vendor;
        if (HasHardwareH264Encoder(&vendor)) {
            // —— 硬件路径：MF H264 编码器 ——
            LOGT(TAG, "[编码] 创建 MF 硬件 H264 编码器 (%s)", vendor.c_str());
            return std::make_unique<MFH264Encoder>();
        }
        // 无硬件：交回内建工厂（OpenH264 软编或协商纠正为 VP8/VP9）
        LOGT(TAG, "[编码] H264 被协商但硬件不可用 → 内建软编兜底");
    }
    // —— 软件兜底路径 ——
    // return builtin_->Create(env, format);
    // —— 软件兜底路径：包一层代理打印实际初始化的类型与分辨率 ——
    auto inner = builtin_->Create(env, format);
    if (inner) return std::make_unique<LogProxyEncoder>(std::move(inner), format.name);
    return nullptr;
}

}  // namespace vm::hw
