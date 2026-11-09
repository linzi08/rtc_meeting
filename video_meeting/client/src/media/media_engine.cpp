#include "media_engine.h"

#include "../util/log.h"
#include "hw_codecs/mf_decoder_factory.h"
#include "hw_codecs/mf_encoder_factory.h"

#include <api/audio_codecs/builtin_audio_decoder_factory.h>
#include <api/audio_codecs/builtin_audio_encoder_factory.h>
#include <api/create_modular_peer_connection_factory.h>
#include <api/enable_media.h>
#include <mfapi.h>
#include <rtc_base/ssl_adapter.h>

#if VM_HW_INJECT
#include <mediasoupclient.hpp>
#endif

namespace vm::media {

static const char* TAG = "media";

MediaEngine& MediaEngine::Instance() {
    static MediaEngine inst;
    return inst;
}

bool MediaEngine::Init(bool enableHwInject, bool preferVp9ForSvc) {
    static bool inited = false;
    if (inited) return true;
    inited = true;

    // webrtc 全局初始化（幂等；m140：Thread/SSL 均已并入 webrtc 命名空间）
    webrtc::InitializeSSL();

    // Media Foundation 全局初始化（编解码 MFT 使用；进程一次）
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr)) LOGT(TAG, "MFStartup 失败 0x%08lX（硬编解将不可用）", hr);

    // 本类 PCF 所用的三个线程（轨道创建在信令线程）
    networkThread_   = webrtc::Thread::CreateWithSocketServer();
    workerThread_    = webrtc::Thread::Create();
    signalingThread_ = webrtc::Thread::Create();
    networkThread_->SetName("vm-net", nullptr);
    workerThread_->SetName("vm-work", nullptr);
    signalingThread_->SetName("vm-sig", nullptr);
    networkThread_->Start();
    workerThread_->Start();
    signalingThread_->Start();

    // —— 本地轨道工厂（模块化创建 + EnableMedia 启用媒体引擎）——
    webrtc::PeerConnectionFactoryDependencies dep;
    dep.network_thread        = networkThread_.get();
    dep.worker_thread         = workerThread_.get();
    dep.signaling_thread      = signalingThread_.get();
    dep.audio_encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
    dep.audio_decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();
    webrtc::EnableMedia(dep);  // m140：媒体引擎默认关闭，必须显式启用
    pcf_ = webrtc::CreateModularPeerConnectionFactory(std::move(dep));
    if (!pcf_) {
        LOGT(TAG, "PeerConnectionFactory 创建失败");
        return false;
    }

    // =====================================================================
    // 【硬编解码注入——硬编优先/软编兜底的总开关，代码显式体现】
    //   优先级（编码）：MF 硬件 H264（Intel QSV/NVIDIA NVENC/AMD VCN）
    //                  > 内建软编 VP8/VP9/OpenH264 兜底（无硬件/未打补丁时自动生效）
    //   优先级（解码）：MF 硬件 H264 > 微软软件 H264 MFT > 内建软解 VP8/VP9 兜底
    // =====================================================================
#if VM_HW_INJECT
    if (enableHwInject) {
        // 组合工厂：在 libmediasoupclient 创建 PCF 时替换其视频工厂（见补丁）
        // auto* enc = new hw::MFBuiltinEncoderFactory(preferVp9ForSvc);
        // auto* dec = new hw::MFBuiltinDecoderFactory();

        // 组合工厂：以"生产回调"注入——每次 libmediasoupclient 创建 PeerConnection
        // 都会调用一次生成新实例（unique_ptr 若只注入实例会被一次性消费掉，
        // Device::Load 内部的临时 pc 会先消费，导致真正发流的 Transport 退回软编）
        std::string vendor;
        if (hw::HasHardwareH264Encoder(&vendor)) {
            LOGT(TAG, "[编码] 硬件 H264 可用（%s）→ 优先级: MF硬编 > 内建软编兜底",
                 vendor.c_str());
        } else {
            LOGT(TAG, "[编码] 未检测到硬件 H264 编码器 → 自动回退内建软编 VP8/VP9/OpenH264");
        }
        LOGT(TAG, "[解码] 优先级: MF硬件(三厂) > 微软软解 MFT > 内建软解 VP8/VP9 兜底");

        // —— 注入（补丁提供的入口；见 patches/README.md）——
        mediasoupclient::PeerConnection::SetVideoCodecFactories(
            [preferVp9ForSvc]() -> std::unique_ptr<webrtc::VideoEncoderFactory> {
                return std::make_unique<hw::MFBuiltinEncoderFactory>(preferVp9ForSvc);
            },
            []() -> std::unique_ptr<webrtc::VideoDecoderFactory> {
                return std::make_unique<hw::MFBuiltinDecoderFactory>();
            });
        LOGT(TAG, "已注入 MF 组合编解码器工厂到 libmediasoupclient（硬编解生效）");
    } else {
        LOGT(TAG, "--no-hw：不注入硬编解码工厂，视频走内建软编 VP8/VP9/OpenH264");
    }
#else
    // 未打补丁模式（CMake -DVM_HW_INJECT=OFF）：libmediasoupclient 使用其内建
    // 软编（VP8/VP9/OpenH264），其余功能（信令/渲染/屏幕共享/重连/TURN）完全不受影响。
    LOGT(TAG, "VM_HW_INJECT=OFF（未打补丁兼容模式）：视频 = 内建软编，SVC 仍可用（走 encodings 注入）");
    (void)enableHwInject;
    (void)preferVp9ForSvc;
#endif
    return true;
}

webrtc::scoped_refptr<webrtc::VideoTrackInterface> MediaEngine::CreateVideoTrack(
    webrtc::scoped_refptr<LocalVideoSource> source, const std::string& id) {
    // m140 签名：CreateVideoTrack(scoped_refptr<VideoTrackSourceInterface>, label)
    return pcf_->CreateVideoTrack(source, id);
}

webrtc::scoped_refptr<webrtc::AudioTrackInterface> MediaEngine::CreateAudioTrack(
    webrtc::scoped_refptr<PcmAudioSource> source, const std::string& id) {
    // m140 签名：CreateAudioTrack(label, AudioSourceInterface*)
    return pcf_->CreateAudioTrack(id, source.get());
}

void MediaEngine::Shutdown() {
    LOGT(TAG, "Shutdown: 媒体引擎开始关闭...");
    pcf_ = nullptr;
    if (signalingThread_) signalingThread_->Stop();
    if (workerThread_) workerThread_->Stop();
    if (networkThread_) networkThread_->Stop();
    signalingThread_.reset();
    workerThread_.reset();
    networkThread_.reset();
    LOGT(TAG, "Shutdown: 媒体引擎已关闭");
}

}  // namespace vm::media