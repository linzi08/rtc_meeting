#include "mf_video_encoder.h"

#include "../../util/log.h"

#include "d3d11_device.h"
#include <api/video/encoded_image.h>
#include <api/video/video_frame_buffer.h>   // NV12BufferInterface / VideoFrameBuffer::Type
#include <libyuv.h>
#include <cstring>                          // memcpy（NV12 直通路径逐行拷贝）
#include <mfidl.h>
#include <modules/video_coding/include/video_codec_interface.h>  // CodecSpecificInfo
#include <modules/video_coding/include/video_error_codes.h>      // WEBRTC_VIDEO_CODEC_*

namespace vm::hw {

static const char* TAG = "codec";

// ---------------------------------------------------------------------------
// 硬件 H264 编码 MFT 枚举（供工厂探测与本类创建共用）
// 返回 false 表示本机无硬件编码器 → 上层自动走内建软编 VP8/VP9 兜底
// ---------------------------------------------------------------------------
bool FindHardwareEncoderActivate(Microsoft::WRL::ComPtr<IMFActivate>* out,
                                 std::string* vendorName) {
    MFT_REGISTER_TYPE_INFO outInfo = {};
    outInfo.guidMajorType = MFMediaType_Video;
    outInfo.guidSubtype   = MFVideoFormat_H264;

    IMFActivate** acts = nullptr;
    UINT32        n    = 0;
    // 硬件 + 异步 MFT + 按质量排序（三厂驱动实现的都在这里）
    UINT32 flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, &outInfo, &acts, &n);
    if (FAILED(hr) || n == 0 || !acts) {
        if (acts) CoTaskMemFree(acts);
        return false;
    }

    // 取第一个（SORTANDFILTER 下即驱动最优实现）
    // acts[0]->AddRef();
    // out->Attach(acts[0]);
    // 优先 NVIDIA（NVENC）：双显卡机器（Intel+NVIDIA）上避免落到 Intel QSV
    UINT32 pick = 0;
    for (UINT32 i = 0; i < n; ++i) {
        WCHAR* nm  = nullptr;
        UINT32 nlen = 0;
        if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &nm, &nlen)) &&
            nm) {
            bool isNv = (wcsstr(nm, L"NVIDIA") != nullptr);
            CoTaskMemFree(nm);
            if (isNv) {
                pick = i;
                break;
            }
        }
    }
    // 无 NVIDIA 时取排序后第一个（Intel QSV / AMD VCN）
    acts[pick]->AddRef();
    out->Attach(acts[pick]);

    // 读取友好名（含厂商信息，如 "NVIDIA H.264 Encoder MFT"）
    if (vendorName) {
        WCHAR* name = nullptr;
        UINT32 len  = 0;
        //if (SUCCEEDED(acts[0]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &len)) &&
        if (SUCCEEDED(acts[pick]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &len)) &&
            name) {
            char buf[128] = {0};
            WideCharToMultiByte(CP_UTF8, 0, name, -1, buf, sizeof(buf), nullptr, nullptr);
            *vendorName = buf;
            CoTaskMemFree(name);
        }
    }

    for (UINT32 i = 0; i < n; ++i) acts[i]->Release();
    CoTaskMemFree(acts);
    return true;
}

namespace {

// 在 Annex-B 码流中找第一个 VCL NAL，判断是否 IDR（type 5）→ webrtc 关键帧
bool IsKeyFrameAnnexB(const uint8_t* p, size_t len) {
    size_t i = 0;
    while (i + 4 < len) {
        if (p[i] == 0 && p[i + 1] == 0 && (p[i + 2] == 1 || (p[i + 2] == 0 && p[i + 3] == 1))) {
            size_t  hdr   = (p[i + 2] == 1) ? 3 : 4;
            uint8_t ntype = 0;
            if (i + hdr < len) ntype = p[i + hdr] & 0x1F;
            if (ntype >= 1 && ntype <= 5) return ntype == 5;  // 找到 VCL NAL
            i += hdr + 1;
        } else {
            ++i;
        }
    }
    return false;
}

}  // namespace

MFH264Encoder::~MFH264Encoder() { Release(); }

// ---------------------------------------------------------------------------
// InitEncode：webrtc 选定 H264 后调用（此时必已探测到硬件，见组合工厂）
// ---------------------------------------------------------------------------
int MFH264Encoder::InitEncode(const webrtc::VideoCodec* codec_settings, const Settings&) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    DestroyMft();

    width_  = codec_settings->width;
    height_ = codec_settings->height;
    fps_    = codec_settings->maxFramerate > 0 ? codec_settings->maxFramerate : 30;
    // startBitrate 单位 kbps；为 0（未定）时按分辨率给经验值
    bitrateBps_ = codec_settings->startBitrate > 0
                      ? (uint32_t)codec_settings->startBitrate * 1000
                      : (uint32_t)(width_ * height_ * 0.12);  // 720p≈2.6Mbps
    if (bitrateBps_ < 300000) bitrateBps_ = 300000;

    if (!CreateAndConfigureMft(width_, height_, fps_, bitrateBps_)) {
        LOGT(TAG, "[编码] 硬件 MFT 初始化失败（组合工厂保证仅在硬件存在时选择 H264）");
        return WEBRTC_VIDEO_CODEC_ERROR;
    }

    running_ = true;
    LOGT(TAG, "[编码] MF 硬件 H264 就绪: %dx%d@%dfps %.1fkbps CBR 低延迟", width_, height_,
         fps_, bitrateBps_ / 1000.0);
    return WEBRTC_VIDEO_CODEC_OK;
}

bool MFH264Encoder::CreateAndConfigureMft(int width, int height, int fps, uint32_t bitrateBps) {
    // 1) 激活硬件 MFT
    Microsoft::WRL::ComPtr<IMFActivate> act;
    std::string vendor;
    if (!FindHardwareEncoderActivate(&act, &vendor)) return false;

    HRESULT hr = act->ActivateObject(IID_PPV_ARGS(&mft_));
    if (FAILED(hr)) return false;
    LOGT(TAG, "[编码] 使用硬件 MFT: %s", vendor.c_str());

    // 2) 异步属性检测（只有真异步 MFT 才走事件节拍，否则等 NeedInput 会永远超时）
    async_          = false;
    needInputMisses_ = 0;
    Microsoft::WRL::ComPtr<IMFAttributes> attrs;
    hr = mft_->GetAttributes(&attrs);
    if (SUCCEEDED(hr)) {
        UINT32 isAsync = 0, d3dAware = 0;
        if (SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync)) && isAsync) {
            async_ = true;
            attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            mft_->QueryInterface(IID_PPV_ARGS(&events_));
        }
        attrs->GetUINT32(MF_SA_D3D11_AWARE, &d3dAware);
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        LOGT(TAG, "[编码] MFT 模式: %s, D3D11Aware=%u",
             async_ ? "异步(事件节拍)" : "同步(直接喂数)", d3dAware);
    }
    // mft_->QueryInterface(IID_PPV_ARGS(&events_));    // 事件发生器（NeedInput/HaveOutput）
    mft_->QueryInterface(IID_PPV_ARGS(&codecApi_));  // 编码参数接口

    // 2.5) 绑定 MF D3D11 设备管理器 → NVENC 在 GPU 上工作（失败降级不影响功能）
    //      注：MFT 会自行 AddRef 该管理器，直到 SET_D3D_MANAGER 传 NULL
    // if (auto mgr = VideoDXGIDeviceManager()) {
    //     if (SUCCEEDED(mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)mgr.Get()))) {
    //         LOGT(TAG, "[编码] 已绑定 D3D11 设备管理器（NVENC GPU 路径）");
    //     } else {
    //         LOGT(TAG, "[编码] 绑定 D3D11 设备管理器失败 → 系统内存路径");
    //     }
    // } else {
    //     LOGT(TAG, "[编码] 无 MF 专用 D3D11 设备 → 系统内存路径");
    // }

    // 2.5) 【重要】不绑定 D3D11 设备管理器
    //      绑定（MFT_MESSAGE_SET_D3D_MANAGER）的语义是"我承诺给你喂 DXGI 纹理"。
    //      一旦绑定，硬件 MFT 会切到 DXGI surface 输入模式，要先在 D3D 设备上分配
    //      纹理池，之后才会发 METransformNeedInput；而 Encode 里喂的是系统内存
    //      NV12（MFCreateMemoryBuffer），与它的期望不符 → MFT 卡在等纹理状态，
    //      既不报错也不发 NeedInput，表现为每帧 "等待 NeedInput 超时"。
    //      阶段 1 走系统内存输入（QSV/NVENC 内部自行上传到 GPU，仍是硬编）；
    //      阶段 2 若改喂纹理，才需要在这里重新绑定。
    LOGT(TAG, "[编码] 输入模式: 系统内存 NV12（未绑定 D3D 管理器）");

    // 3) 输出类型：H264
    Microsoft::WRL::ComPtr<IMFMediaType> out;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, fps, 1);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    out->SetUINT32(MF_MT_AVG_BITRATE, bitrateBps);
    hr = mft_->SetOutputType(0, out.Get(), 0);
    if (FAILED(hr)) {
        // 硬件 MFT 常只接受它自己枚举出的输出类型 → 退回可用类型
        Microsoft::WRL::ComPtr<IMFMediaType> avail;
        if (SUCCEEDED(mft_->GetOutputAvailableType(0, 0, &avail)) && avail &&
            SUCCEEDED(mft_->SetOutputType(0, avail.Get(), 0))) {
            LOGT(TAG, "[编码] SetOutputType 自定义失败(0x%08lX) → 改用 MFT 可用类型", hr);
        } else {
            LOGT(TAG, "[编码] SetOutputType(H264) 失败 0x%08lX", hr);
            return false;
        }
    }

    
    // 4) 输入类型：NV12（硬件编码器统一输入格式）
    Microsoft::WRL::ComPtr<IMFMediaType> in;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = mft_->SetInputType(0, in.Get(), 0);
    if (FAILED(hr)) {
        // 列出该 MFT 真实接受的输入类型，辅助定位（0x3231564E=NV12 0x30323449=I420）
        for (DWORD i = 0;; ++i) {
            Microsoft::WRL::ComPtr<IMFMediaType> avail;
            if (FAILED(mft_->GetInputAvailableType(0, i, &avail)) || !avail) break;
            GUID sub = GUID_NULL;
            avail->GetGUID(MF_MT_SUBTYPE, &sub);
            LOGT(TAG, "[编码] 可用输入类型[%lu] subtype=0x%08lX", (unsigned long)i, sub.Data1);
        }
        LOGT(TAG, "[编码] SetInputType(NV12) 失败 0x%08lX", hr);
        return false;
    }

    // 4.5) 输出/输入流信息：判断 MFT 是否自带输出样本（决定 ProcessOutput 是否要预分配）
    MFT_OUTPUT_STREAM_INFO osi = {};
    if (SUCCEEDED(mft_->GetOutputStreamInfo(0, &osi))) {
        providesSamples_ = (osi.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
        outSampleSize_   = osi.cbSize ? osi.cbSize : (uint32_t)((size_t)width * height * 3 / 2);
        LOGT(TAG, "[编码] 输出流 flags=0x%08lX cbSize=%lu providesSamples=%d",
             (unsigned long)osi.dwFlags, (unsigned long)osi.cbSize, providesSamples_ ? 1 : 0);
    }
    MFT_INPUT_STREAM_INFO isi = {};
    if (SUCCEEDED(mft_->GetInputStreamInfo(0, &isi))) {
        LOGT(TAG, "[编码] 输入流 flags=0x%08lX cbSize=%lu", (unsigned long)isi.dwFlags,
             (unsigned long)isi.cbSize);
    }

    // 5) ICodecAPI 编码参数（每项独立容错：个别驱动不支持不影响整体启动）
    SetCodecApiU32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
    SetCodecApiU32(CODECAPI_AVEncCommonMeanBitRate, bitrateBps);
    SetCodecApiU32(CODECAPI_AVEncCommonLowLatency, 1);  // 低延迟（驱动支持则生效）
    SetCodecApiU32(CODECAPI_AVEncCommonRealTime, 1);
    SetCodecApiU32(CODECAPI_AVEncMPVGOPSize, (uint32_t)(fps * 3));  // 关键帧间隔 ≈3s

    // 6) 启动流（HRESULT 必须检查：启动失败只会表现为“永不发 NeedInput”）
    hr = mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    LOGT(TAG, "[编码] BEGIN_STREAMING hr=0x%08lX", hr);
    hr = mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    LOGT(TAG, "[编码] START_OF_STREAM hr=0x%08lX", hr);
    return true;
}

void MFH264Encoder::DestroyMft() {
    if (mft_) {
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    codecApi_.Reset();
    events_.Reset();
    mft_.Reset();
    running_ = false;
}

bool MFH264Encoder::SetCodecApiU32(const GUID& guid, uint32_t v) {
    if (!codecApi_) return false;
    VARIANT vt;
    VariantInit(&vt);
    vt.vt    = VT_UI4;
    vt.ulVal = v;
    return SUCCEEDED(codecApi_->SetValue(&guid, &vt));
}

// 轮询等待异步事件（预算制；不永久阻塞）
// 异步事件泵：收到什么事件就做什么，避免“等 A 事件时把 B 事件吃掉”。
//   METransformNeedInput  → ProcessInput（喂入成功立即返回）
//   METransformHaveOutput  → ProcessOutput 取走编码数据并回调
//   MEError                → 打印驱动真实失败码
// 喂入即返回（输出留给下一轮事件取）→ 天然 1 帧流水线，延迟最低。
bool MFH264Encoder::PumpEncode(IMFSample* input, int budgetMs) {
    if (!events_) return false;  // 异步接口未拿到（QI 失败）时不能裸解引用
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    for (;;) {
        Microsoft::WRL::ComPtr<IMFMediaEvent> ev;
        HRESULT hr = events_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
        if (SUCCEEDED(hr)) {
            MediaEventType type = MEUnknown;
            ev->GetType(&type);
            if (type == METransformNeedInput) {
                HRESULT in = mft_->ProcessInput(0, input, 0);
                if (SUCCEEDED(in)) {
                    ++framesIn_;
                    DrainOutputs(0);  // 顺手取走已就绪的输出（非阻塞）
                    return true;
                }
                if (in != MF_E_NOTACCEPTING) {
                    LOGT(TAG, "[编码] ProcessInput 失败 0x%08lX%s", in,
                         in == MF_E_TRANSFORM_TYPE_NOT_SET ? "（输入类型失效，需重建 MFT）" : "");
                    return false;
                }
            } else if (type == METransformHaveOutput) {
                DrainOutputs(0);  // 输出就绪：取走并回调
            } else if (type == MEError) {
                HRESULT st = S_OK;
                ev->GetStatus(&st);
                LOGT(TAG, "[编码] MFT 错误事件 status=0x%08lX", st);
                return false;
            }
            // Sleep(1);
            continue;
        }
        if (hr != MF_E_NO_EVENTS_AVAILABLE) {
            LOGT(TAG, "[编码] GetEvent 失败 0x%08lX", hr);
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) return false;  // 预算耗尽
        Sleep(1);
    }
}

int32_t MFH264Encoder::RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) {
    callback_ = callback;
    return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MFH264Encoder::Release() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    DrainOutputs(100);  // 尽量取走已编码数据
    DestroyMft();
    return WEBRTC_VIDEO_CODEC_OK;
}

// ---------------------------------------------------------------------------
// Encode：I420 → NV12 → MFT → EncodedImage
// ---------------------------------------------------------------------------
int32_t MFH264Encoder::Encode(const webrtc::VideoFrame& frame,
                              const std::vector<webrtc::VideoFrameType>* frame_types) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!running_ || !callback_) return WEBRTC_VIDEO_CODEC_ERROR;

    lastRtpTs_ = frame.rtp_timestamp();
    captureTimeMs_ = (int64_t)frame.rtp_timestamp() * 1000LL / 90000LL;

    // 1) webrtc 请求关键帧（PLI/新消费者）→ 通知驱动强制 IDR
    bool forceKey = frame_types && !frame_types->empty() &&
                    frame_types->front() == webrtc::VideoFrameType::kVideoFrameKey;
    if (forceKey) SetCodecApiU32(CODECAPI_AVEncVideoForceKeyFrame, 1);

    // 2) 取源帧：上游若已是 NV12（原生 NV12 采集 / NVDEC 硬解帧），直接走直通路径；
    //    否则转 I420 再用 libyuv 转 NV12。MF 硬件编码 MFT 输入只接受 NV12。
    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> vfb = frame.video_frame_buffer();
    if (!vfb) return WEBRTC_VIDEO_CODEC_ERROR;
    const bool srcNv12 = vfb->type() == webrtc::VideoFrameBuffer::Type::kNV12;

    webrtc::scoped_refptr<const webrtc::I420BufferInterface> i420;
    const webrtc::NV12BufferInterface* nv12In = nullptr;
    if (srcNv12) {
        nv12In = static_cast<const webrtc::NV12BufferInterface*>(vfb.get());
    } else {
        i420 = vfb->ToI420();
        if (!i420) return WEBRTC_VIDEO_CODEC_ERROR;
    }
 
    const int w = srcNv12 ? nv12In->width() : i420->width();
    const int h = srcNv12 ? nv12In->height() : i420->height();
    if (w != width_ || h != height_) {
        // webrtc 自适应降分辨率 → 重建 MFT
        width_  = w;
        height_ = h;
        if (!CreateAndConfigureMft(w, h, fps_, bitrateBps_)) return WEBRTC_VIDEO_CODEC_ERROR;
    }

    const size_t nv12Len = (size_t)w * h + (size_t)w * ((h + 1) / 2);  // Y + UV 行数精确计算
    // std::vector<uint8_t> nv12(nv12Len);
    // uint8_t* y  = nv12.data();
    // uint8_t* uv = nv12.data() + (size_t)w * h;
    // libyuv::I420ToNV12(i420->DataY(), i420->StrideY(), i420->DataU(), i420->StrideU(),
    //                    i420->DataV(), i420->StrideV(), y, w, uv, w, w, h);

    // 3) 包 IMFSample（时间用 RTP 时间戳换算，只需单调递增）
    //    libyuv 直接写入 IMFMediaBuffer 锁定指针，省掉 vector 中转与整帧 memcpy
    Microsoft::WRL::ComPtr<IMFSample>     sample;
    Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
    // MFCreateSample(&sample);
    // MFCreateMemoryBuffer((DWORD)nv12Len, &buf);
    if (FAILED(MFCreateSample(&sample))) return WEBRTC_VIDEO_CODEC_ERROR;
    if (FAILED(MFCreateMemoryBuffer((DWORD)nv12Len, &buf))) return WEBRTC_VIDEO_CODEC_ERROR;
    BYTE* p = nullptr;
    // buf->Lock(&p, nullptr, nullptr);
    // memcpy(p, nv12.data(), nv12Len);
    if (FAILED(buf->Lock(&p, nullptr, nullptr)) || !p) return WEBRTC_VIDEO_CODEC_ERROR;
    if (srcNv12) {
        // NV12 直通：目标缓冲紧密排列（stride==w），源 stride 可能带行尾 padding → 逐行拷贝
        const uint8_t* sy  = nv12In->DataY();
        const uint8_t* suv = nv12In->DataUV();
        const int syStride  = nv12In->StrideY();
        const int suvStride = nv12In->StrideUV();
        const int ch = nv12In->ChromaHeight();
        for (int r = 0; r < h; ++r)
            memcpy(p + (size_t)r * w, sy + (size_t)r * syStride, (size_t)w);
        for (int r = 0; r < ch; ++r)
            memcpy(p + (size_t)w * h + (size_t)r * w, suv + (size_t)r * suvStride, (size_t)w);
    } else {
        libyuv::I420ToNV12(i420->DataY(), i420->StrideY(), i420->DataU(), i420->StrideU(),
                           i420->DataV(), i420->StrideV(), p, w, p + (size_t)w * h, w, w, h);
    }
    buf->Unlock();
    buf->SetCurrentLength((DWORD)nv12Len);
    sample->AddBuffer(buf.Get());
    int64_t t100 = (int64_t)frame.rtp_timestamp() * 1000000LL / 90000LL;  // 90kHz → 100ns
    static int64_t s_last100 = 0;
    if (t100 <= s_last100) t100 = s_last100 + 333333;  // 保证严格递增
    s_last100 = t100;
    sample->SetSampleTime(t100);
    sample->SetSampleDuration(10000000LL / (fps_ > 0 ? fps_ : 30));

    // 4) 异步 MFT：事件泵喂数（收到 NeedInput 才喂）；同步 MFT 直接喂
    if (async_) {
        if (!PumpEncode(sample.Get(), 15)) {
            if (++needInputMisses_ == 1 || needInputMisses_ % 60 == 0)
                LOGT(TAG, "[编码] 未取到 NeedInput（累计 %d 次），丢帧保实时", needInputMisses_);
            return WEBRTC_VIDEO_CODEC_OK;
        }
        needInputMisses_ = 0;
    } else {
        HRESULT hrIn = mft_->ProcessInput(0, sample.Get(), 0);
        if (hrIn == MF_E_NOTACCEPTING) return WEBRTC_VIDEO_CODEC_OK;  // 忙：丢帧
        if (FAILED(hrIn)) {
            LOGT(TAG, "[编码] ProcessInput 失败 0x%08lX", hrIn);
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        DrainOutputs(10);
    }

    return WEBRTC_VIDEO_CODEC_OK;
}

// STREAM_CHANGE：MFT 改了输出类型，客户端必须重设，否则它卡住不再发 NeedInput
bool MFH264Encoder::HandleStreamChange() {
    Microsoft::WRL::ComPtr<IMFMediaType> out;
    for (DWORD i = 0; i < 8; ++i) {
        out.Reset();
        if (FAILED(mft_->GetOutputAvailableType(0, i, &out)) || !out) break;
        GUID   sub = GUID_NULL;
        UINT32 w = 0, h = 0, avg = 0, frN = 0, frD = 0;
        out->GetGUID(MF_MT_SUBTYPE, &sub);
        MFGetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, &frN, &frD);
        out->GetUINT32(MF_MT_AVG_BITRATE, &avg);
        LOGT(TAG, "[编码] STREAM_CHANGE 可用输出类型[%lu]: subtype=0x%08lX %ux%u %u/%ufps avg=%u",
             (unsigned long)i, sub.Data1, w, h, frN, frD, avg);
        if (SUCCEEDED(mft_->SetOutputType(0, out.Get(), 0))) {
            MFT_OUTPUT_STREAM_INFO osi = {};
            if (SUCCEEDED(mft_->GetOutputStreamInfo(0, &osi))) {
                providesSamples_ = (osi.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
                if (osi.cbSize) outSampleSize_ = osi.cbSize;
            }
            LOGT(TAG, "[编码] 输出类型已重设，继续编码");

            // 【重要】此处绝不能重设输入类型！
            //   MFT 已处于流式状态（BEGIN_STREAMING 后），SetInputType 会要求驱动
            //   重建内部编码会话，Intel QSV MFT 会阻塞在驱动内（表现为卡死），
            //   严重时驱动内访问违规直接终止进程。STREAM_CHANGE 只要求重设输出类型。
            //   输入类型若真失效，ProcessInput 会返回 MF_E_TRANSFORM_TYPE_NOT_SET。
            Microsoft::WRL::ComPtr<IMFMediaType> curIn;
            HRESULT hrc = mft_->GetInputCurrentType(0, &curIn);
            if (FAILED(hrc)) LOGT(TAG, "[编码] 输入类型已失效 0x%08lX", hrc);
            return true;
        }
    }
    LOGT(TAG, "[编码] STREAM_CHANGE 重设输出类型失败");
    return false;
}

void MFH264Encoder::DrainOutputs(int budgetMs) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    int  streamChanges = 0;  // STREAM_CHANGE 重协商次数（防死循环）
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER ob = {};
        DWORD         status = 0;
        Microsoft::WRL::ComPtr<IMFSample> ownSample;
        if (!providesSamples_) {
            // MFT 不自带输出样本 → 必须由调用方预分配，否则 ProcessOutput 静默失败
            MFCreateSample(&ownSample);
            Microsoft::WRL::ComPtr<IMFMediaBuffer> mb;
            MFCreateMemoryBuffer(outSampleSize_ ? outSampleSize_ : (1u << 20), &mb);
            ownSample->AddBuffer(mb.Get());
            ob.pSample = ownSample.Get();
        }
        HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &status);
        if (SUCCEEDED(hr)) {
            if (ob.pSample) {
                DeliverSample(ob.pSample);
                if (!ownSample) ob.pSample->Release();
            }
            if (ob.pEvents) ob.pEvents->Release();
            if (std::chrono::steady_clock::now() >= deadline) return;
            continue;
        }
        if (ob.pEvents) ob.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return;   // 正常：暂无更多输出
        // if (hr == MF_E_TRANSFORM_STREAM_CHANGE) continue;   // 极少：格式变化，重试
       // return;  // 其它错误：停止收割（帧丢失但不崩）

       if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {           // 输出类型变更：必须重协商
            if (streamChanges++ >= 3 || !HandleStreamChange()) {
                LOGT(TAG, "[编码] STREAM_CHANGE 处理失败，停止收割");
                return;
            }
            continue;  // 重设后立即重试，把已编码样本取回来
        }
        if (hr == MF_E_BUFFERTOOSMALL && std::chrono::steady_clock::now() < deadline) {
            outSampleSize_ = outSampleSize_ ? outSampleSize_ * 2 : (1u << 20);
            LOGT(TAG, "[编码] 输出缓冲不足 → 增大到 %u", outSampleSize_);
            continue;
        }
        if (hr == E_UNEXPECTED) return;  // 异步 MFT：输出尚未就绪，等 HaveOutput 事件再取
        LOGT(TAG, "[编码] ProcessOutput 失败 0x%08lX status=0x%08lX", hr, status);
        return;
    }
}

void MFH264Encoder::DeliverSample(IMFSample* sample) {
    if (!callback_) return;

    Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
    if (FAILED(sample->ConvertToContiguousBuffer(&buf))) return;
    BYTE* p   = nullptr;
    DWORD len = 0;
    if (FAILED(buf->Lock(&p, nullptr, &len)) || len == 0) {
        if (p) buf->Unlock();
        return;
    }

    bool key = IsKeyFrameAnnexB(p, len);

    // 拷贝到 webrtc 的编码缓冲（编码器内存 → RTP 发送路径，不可避免的一次拷贝）
    auto eb = webrtc::EncodedImageBuffer::Create(len);
    memcpy(eb->data(), p, len);
    buf->Unlock();

    webrtc::EncodedImage image;
    image.SetEncodedData(eb);                      // size() 随缓冲自动确定
    image.SetRtpTimestamp(lastRtpTs_);
    image.SetFrameType(key ? webrtc::VideoFrameType::kVideoFrameKey
                           : webrtc::VideoFrameType::kVideoFrameDelta);
    image._encodedWidth    = (uint32_t)width_;
    image._encodedHeight   = (uint32_t)height_;
    image.capture_time_ms_ = captureTimeMs_;

    // 出流统计前置（回调可能阻塞，先落日志才能定位）
    ++framesOut_;
    if (key) ++keysOut_;
    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (statTickMs_ == 0) {
        statTickMs_ = nowMs;
        LOGT(TAG, "[编码] 首帧输出 %u 字节 %s（输入累计 %llu）", (unsigned)len,
             key ? "IDR关键帧" : "P增量帧", (unsigned long long)framesIn_);
    } else {
        ++statFrames_;
        statBytes_ += len;
        if (nowMs - statTickMs_ >= 1000) {
            double sec = (nowMs - statTickMs_) / 1000.0;
            LOGT(TAG, "[编码] 出流统计: %.1f fps, %.0f kbps, 累计 %llu 帧(关键帧 %llu), 输入 %llu",
                 statFrames_ / sec, statBytes_ * 8.0 / sec / 1000.0,
                 (unsigned long long)framesOut_, (unsigned long long)keysOut_,
                 (unsigned long long)framesIn_);
            statFrames_ = 0;
            statBytes_  = 0;
            statTickMs_ = nowMs;
        }
    }

    // 必须传 CodecSpecificInfo：RtpVideoSender 按 SDP 的 codec 选打包器（H264），
    // RtpPacketizer::Create 会 std::get<RTPVideoHeaderH264>(variant)；
    // 传 nullptr 时 RtpPayloadParams 只走 GenericToGeneric，variant 里是
    // LegacyGeneric → std::get 类型不匹配 → libc++ 断言 abort（无异常无日志）。
    webrtc::CodecSpecificInfo csi;
    csi.codecType                              = webrtc::kVideoCodecH264;
    csi.codecSpecific.H264.packetization_mode  = webrtc::H264PacketizationMode::NonInterleaved;
    csi.codecSpecific.H264.temporal_idx        = webrtc::kNoTemporalIdx;
    csi.codecSpecific.H264.base_layer_sync     = false;
    csi.codecSpecific.H264.idr_frame           = key;
    csi.end_of_picture                         = true;
    callback_->OnEncodedImage(image, &csi);
}

// 动态码率（webrtc 带宽估计 → 立即生效到驱动）
void MFH264Encoder::SetRates(const RateControlParameters& parameters) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    uint32_t bps = parameters.bitrate.get_sum_bps();
    if (bps == 0 || !running_) return;
    if (labs((long)bps - (long)bitrateBps_) < (long)(bitrateBps_ / 10)) return;  // <10% 忽略
    bitrateBps_ = bps;
    SetCodecApiU32(CODECAPI_AVEncCommonMeanBitRate, bps);
}

// ---------------------------------------------------------------------------
// 能力上报（m140 必须实现；supports_native_handle=false → webrtc 喂 I420 帧）
// ---------------------------------------------------------------------------
webrtc::VideoEncoder::EncoderInfo MFH264Encoder::GetEncoderInfo() const {
    webrtc::VideoEncoder::EncoderInfo info;
    info.implementation_name    = "MF-H264-HW";
    info.is_hardware_accelerated = true;
    info.supports_native_handle = false;
    return info;
}

}  // namespace vm::hw
