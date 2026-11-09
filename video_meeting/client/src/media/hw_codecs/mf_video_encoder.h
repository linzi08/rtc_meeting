// =============================================================================
// Media Foundation H.264 硬件编码器（webrtc::VideoEncoder 实现，m140 签名）
//
// 【硬编优先策略——本类只在"探测到硬件 MFT"时才会被创建】
//   覆盖 Intel QuickSync / NVIDIA NVENC / AMD VCN：三厂驱动都会向系统注册
//   MF 硬件编码 MFT（MFT_ENUM_FLAG_HARDWARE），一套代码覆盖三厂。
//   未探测到硬件时，组合工厂（MFBuiltinEncoderFactory）不提供 H264 格式，
//   webrtc 自动协商 VP8/VP9 软编兜底——永不断流。
//
// 编码流水线（低延迟）：
//   I420 帧 →(libyuv SIMD 转换)→ NV12 → MF 硬件编码 MFT
//   配置：MF_LOW_LATENCY（逐帧输出）+ CBR + GOP≈3s + 响应 PLI 强制 IDR
//
// MFT 交互（异步硬件 MFT 标准用法）：
//   MF_TRANSFORM_ASYNC_UNLOCK 解锁后按事件节拍喂数/取数：
//   METransformNeedInput → ProcessInput；METransformHaveOutput → ProcessOutput。
// =============================================================================
#pragma once

#include <api/video/video_frame.h>
#include <api/video_codecs/video_encoder.h>

#include <codecapi.h>
#include <icodecapi.h>   // ICodecAPI 接口（SDK 10.0.19041+ 从 codecapi.h 拆出）
#include <mfapi.h>
#include <mferror.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <mutex>
#include <string>
#include <vector>

namespace vm::hw {

class MFH264Encoder : public webrtc::VideoEncoder {
public:
    MFH264Encoder() = default;
    ~MFH264Encoder() override;

    // —— webrtc::VideoEncoder 标准接口（m140：InitEncode 两参数版）——
    int InitEncode(const webrtc::VideoCodec* codec_settings,
                   const Settings&           settings) override;
    int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) override;
    int32_t Release() override;
    int32_t Encode(const webrtc::VideoFrame&                             frame,
                   const std::vector<webrtc::VideoFrameType>* frame_types) override;
    void    SetRates(const RateControlParameters& parameters) override;

    // m140 纯虚：能力上报（is_hardware_accelerated=true 让统计可见）
    EncoderInfo GetEncoderInfo() const override;

private:
    // 创建并配置硬件 MFT（类型协商 + ICodecAPI 参数 + 启动流）
    bool CreateAndConfigureMft(int width, int height, int fps, uint32_t bitrateBps);
    void DestroyMft();

    // 异步事件泵：NeedInput→喂数、HaveOutput→取数、STREAM_CHANGE→重协商
    // 预算制，绝不永久阻塞 webrtc 编码线程
    bool PumpEncode(IMFSample* input, int budgetMs);

    // 把当前输出全部收割并回调给 webrtc
    void DrainOutputs(int budgetMs);

    // 输出类型变更（MF_E_TRANSFORM_STREAM_CHANGE）：重新枚举并重设输出类型
    bool HandleStreamChange();

    // 单个编码样本 → EncodedImage（解析 NAL 类型：IDR=关键帧）→ OnEncodedImage
    void DeliverSample(IMFSample* sample);

    bool SetCodecApiU32(const GUID& guid, uint32_t v);  // 驱动参数设置（单项失败不致命）

    Microsoft::WRL::ComPtr<IMFTransform>           mft_;
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> events_;   // 异步 MFT 事件
    Microsoft::WRL::ComPtr<ICodecAPI>              codecApi_; // 码率/GOP/强制 IDR

    webrtc::EncodedImageCallback* callback_ = nullptr;

    int      width_ = 0, height_ = 0, fps_ = 30;
    uint32_t bitrateBps_ = 1500000;
    uint32_t lastRtpTs_  = 0;   // Encode 时记录，DeliverSample 回填
    int64_t  captureTimeMs_ = 0;  // 采集时间(ms)，填 EncodedImage.capture_time_ms_
    uint64_t framesIn_  = 0;   // ProcessInput 成功次数
    uint64_t framesOut_ = 0;   // 成功回调的编码帧数
    uint64_t keysOut_   = 0;   // 其中关键帧数
    uint64_t statFrames_ = 0;  // 当前统计窗口内的帧数
    uint64_t statBytes_  = 0;  // 当前统计窗口内的字节数
    int64_t  statTickMs_ = 0;  // 上次统计时间（steady_clock 毫秒）
    bool     running_    = false;
    bool     async_      = false;   // MFT 是否真异步（读 MF_TRANSFORM_ASYNC）
    int      needInputMisses_ = 0;  // 未取到 NeedInput 的次数（仅用于日志节流）
    bool     providesSamples_  = false;  // MFT 是否自带输出样本
    uint32_t outSampleSize_    = 0;      // 调用方分配输出样本时的缓冲大小
    std::recursive_mutex mutex_;  // OnEncodedImage 回调可能重入 SetRates，递归锁防自锁
};

}  // namespace vm::hw
