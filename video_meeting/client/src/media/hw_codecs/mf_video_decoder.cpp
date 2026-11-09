#include "mf_video_decoder.h"

#include "../../util/log.h"

#include <api/video/nv12_buffer.h>
#include <mfidl.h>
#include <modules/video_coding/include/video_error_codes.h>  // WEBRTC_VIDEO_CODEC_*

namespace vm::hw {

static const char* TAG = "codec";

namespace {

// ---------------------------------------------------------------------------
// 解码 MFT 枚举
//   hardware=true : MFT_ENUM_FLAG_HARDWARE | ASYNCMFT（三厂 DXVA 实现）
//   hardware=false: MFT_ENUM_FLAG_SYNCMFT（微软软件 H264 解码器，Windows 自带）
// ---------------------------------------------------------------------------
bool EnumDecoderActivates(bool hardware, std::vector<Microsoft::WRL::ComPtr<IMFActivate>>* out) {
    MFT_REGISTER_TYPE_INFO inInfo = {};
    inInfo.guidMajorType = MFMediaType_Video;
    inInfo.guidSubtype   = MFVideoFormat_H264;

    UINT32 flags = hardware ? (MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT |
                               MFT_ENUM_FLAG_SORTANDFILTER)
                            : (MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER);

    IMFActivate** acts = nullptr;
    UINT32        n    = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, flags, &inInfo, nullptr, &acts, &n)) ||
        n == 0 || !acts) {
        if (acts) CoTaskMemFree(acts);
        return false;
    }
    // 收集所有候选，并把 NVIDIA（NVDEC）排到最前
    std::vector<std::pair<bool, Microsoft::WRL::ComPtr<IMFActivate>>> tmp;
    for (UINT32 i = 0; i < n; ++i) {
        bool   isNv = false;
        WCHAR* nm   = nullptr;
        UINT32 nlen = 0;
        if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &nm, &nlen)) &&
            nm) {
            isNv = (wcsstr(nm, L"NVIDIA") != nullptr);
            CoTaskMemFree(nm);
        }
        Microsoft::WRL::ComPtr<IMFActivate> a;
        acts[i]->AddRef();
        a.Attach(acts[i]);
        // out->push_back(a);
        acts[i]->Release();
    }
    for (auto& p : tmp)
        if (p.first) out->push_back(p.second);
    for (auto& p : tmp)
        if (!p.first) out->push_back(p.second);
        
    CoTaskMemFree(acts);
    return !out->empty();
}

std::string FriendlyName(IMFActivate* act) {
    WCHAR* name = nullptr;
    UINT32 len  = 0;
    if (SUCCEEDED(act->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &len)) && name) {
        char buf[128] = {0};
        WideCharToMultiByte(CP_UTF8, 0, name, -1, buf, sizeof(buf), nullptr, nullptr);
        CoTaskMemFree(name);
        return buf;
    }
    return "unknown";
}

}  // namespace

MFH264Decoder::~MFH264Decoder() { Release(); }

// ---------------------------------------------------------------------------
// Configure：两级初始化——硬件优先，软件兜底（决策显式写在这里）
// ---------------------------------------------------------------------------
bool MFH264Decoder::Configure(const Settings&) {
    std::lock_guard<std::mutex> lock(mutex_);
    DestroyMft();
    if (InitMft(/*preferHardware=*/true)) {
        LOGT(TAG, "[解码] 使用硬件解码 MFT: %s（DXVA/D3D11）", backendName_.c_str());
    } else {
        LOGT(TAG, "[解码] 硬件 MFT 不可用 → 回退微软软件 H264 解码 MFT（兜底路径）");
        DestroyMft();
        if (!InitMft(false)) {
            LOGT(TAG, "[解码] 软件 MFT 也失败（异常，Windows 应自带）");
            return false;
        }
        LOGT(TAG, "[解码] 使用软件解码 MFT: %s", backendName_.c_str());
    }
    running_ = true;
    return true;
}

bool MFH264Decoder::InitMft(bool preferHardware) {
    std::vector<Microsoft::WRL::ComPtr<IMFActivate>> acts;
    if (!EnumDecoderActivates(preferHardware, &acts)) return false;

    HRESULT hr = E_FAIL;
    for (auto& act : acts) {
        hr = act->ActivateObject(IID_PPV_ARGS(&mft_));
        if (SUCCEEDED(hr)) {
            backendName_ = "MF:" + FriendlyName(act.Get());
            break;
        }
        mft_.Reset();
    }
    if (FAILED(hr) || !mft_) return false;

    // 异步 MFT：解锁 + 事件接口（硬件解码器通常是异步的）
    Microsoft::WRL::ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(mft_->GetAttributes(&attrs))) {
        UINT32 isAsync = 0;
        if (SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync)) && isAsync) {
            attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            mft_->QueryInterface(IID_PPV_ARGS(&events_));
        }
    }

    // 输入类型：H264（不预设分辨率——分辨率随流变化，靠 STREAM_CHANGE 重协商）
    Microsoft::WRL::ComPtr<IMFMediaType> in;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (FAILED(mft_->SetInputType(0, in.Get(), 0))) return false;

    // 输出类型：优先 NV12（个别流要先喂数据才知道尺寸 → HandleStreamChange 兜底）
    Microsoft::WRL::ComPtr<IMFMediaType> out;
    for (DWORD i = 0;; ++i) {
        Microsoft::WRL::ComPtr<IMFMediaType> t;
        if (FAILED(mft_->GetOutputAvailableType(0, i, &t))) break;
        GUID sub{};
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        if (sub == MFVideoFormat_NV12) {
            out = t;
            break;
        }
    }
    if (out) {
        mft_->SetOutputType(0, out.Get(), 0);
    } else {
        // 个别硬件解码器喂首帧前拿不到 NV12：先设第一个可用类型，靠 STREAM_CHANGE 修正
        Microsoft::WRL::ComPtr<IMFMediaType> any;
        if (SUCCEEDED(mft_->GetOutputAvailableType(0, 0, &any)))
            mft_->SetOutputType(0, any.Get(), 0);
    }

    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    return true;
}

void MFH264Decoder::DestroyMft() {
    if (mft_) mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    events_.Reset();
    mft_.Reset();
    running_ = false;
}

int32_t MFH264Decoder::RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback* callback) {
    callback_ = callback;
    return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MFH264Decoder::Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    DestroyMft();
    return WEBRTC_VIDEO_CODEC_OK;
}

bool MFH264Decoder::WaitForEvent(MediaEventType want, int budgetMs) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    for (;;) {
        Microsoft::WRL::ComPtr<IMFMediaEvent> ev;
        HRESULT hr = events_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
        if (SUCCEEDED(hr)) {
            MediaEventType type = MEUnknown;
            ev->GetType(&type);
            if (type == want) return true;
            continue;
        }
        if (hr == MF_E_NO_EVENTS_AVAILABLE) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            Sleep(1);
            continue;
        }
        return false;
    }
}

bool MFH264Decoder::CurrentFrameSize(int* w, int* h) {
    Microsoft::WRL::ComPtr<IMFMediaType> mt;
    if (FAILED(mft_->GetOutputCurrentType(0, &mt))) return false;
    UINT32 uw = 0, uh = 0;
    if (FAILED(MFGetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, &uw, &uh))) return false;
    *w = (int)uw;
    *h = (int)uh;
    return *w > 0 && *h > 0;
}

void MFH264Decoder::HandleStreamChange() {
    // 重新协商 NV12 输出（分辨率可能已变，取 MFT 给的可用类型中第一个 NV12）
    for (DWORD i = 0;; ++i) {
        Microsoft::WRL::ComPtr<IMFMediaType> t;
        if (FAILED(mft_->GetOutputAvailableType(0, i, &t))) break;
        GUID sub{};
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        if (sub == MFVideoFormat_NV12) {
            mft_->SetOutputType(0, t.Get(), 0);
            int w, h;
            if (CurrentFrameSize(&w, &h)) LOGT(TAG, "[解码] 流尺寸变化 → %dx%d (NV12)", w, h);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Decode：AnnexB 码流 → MFT → NV12Buffer → Decoded 回调
// ---------------------------------------------------------------------------
int32_t MFH264Decoder::Decode(const webrtc::EncodedImage& input_image, bool, int64_t) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || !callback_) return WEBRTC_VIDEO_CODEC_ERROR;

    const uint8_t* data = input_image.data();
    const size_t   size = input_image.size();
    if (!data || size == 0) return WEBRTC_VIDEO_CODEC_ERROR;

    // 1) 包输入样本
    Microsoft::WRL::ComPtr<IMFSample>     sample;
    Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
    MFCreateSample(&sample);
    MFCreateMemoryBuffer((DWORD)size, &buf);
    BYTE* p = nullptr;
    buf->Lock(&p, nullptr, nullptr);
    memcpy(p, data, size);
    buf->Unlock();
    buf->SetCurrentLength((DWORD)size);
    sample->AddBuffer(buf.Get());
    int64_t t100 = (int64_t)input_image.RtpTimestamp() * 1000000LL / 90000LL;
    if (t100 <= lastTsUs_ * 10) t100 = lastTsUs_ * 10 + 3333;  // 单调递增（100ns）
    sample->SetSampleTime(t100);

    // 2) 喂数：异步 MFT 等事件；同步（微软软解）直接喂
    if (IsAsync()) {
        if (!WaitForEvent(METransformNeedInput, 50))
            return WEBRTC_VIDEO_CODEC_OK;  // 忙：丢弃本帧保实时（解码器落后时合理）
    }
    if (FAILED(mft_->ProcessInput(0, sample.Get(), 0))) return WEBRTC_VIDEO_CODEC_ERROR;

    // 3) 收割输出（可能一帧输入触发多次输出，也可能暂无输出）
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER ob = {};
        DWORD         status = 0;
        HRESULT       hr     = mft_->ProcessOutput(0, 1, &ob, &status);
        if (SUCCEEDED(hr)) {
            if (ob.pSample) {
                DeliverNV12(ob.pSample);
                ob.pSample->Release();
            }
            if (ob.pEvents) ob.pEvents->Release();
            continue;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            HandleStreamChange();  // 分辨率/格式变化
            continue;
        }
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            // 异步模式：也许 HaveOutput 事件稍后才到，小预算再等一轮
            if (IsAsync() && std::chrono::steady_clock::now() < deadline &&
                WaitForEvent(METransformHaveOutput, 10))
                continue;
            return WEBRTC_VIDEO_CODEC_OK;
        }
        return WEBRTC_VIDEO_CODEC_ERROR;
    }
}

void MFH264Decoder::DeliverNV12(IMFSample* s) {
    Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
    if (FAILED(s->ConvertToContiguousBuffer(&buf))) return;

    // Lock2D 优先（硬件样本可能是 2D 平面）；失败退普通 Lock（连续内存）
    BYTE* p        = nullptr;
    LONG  pitchY   = 0;
    bool  locked2d = false;
    Microsoft::WRL::ComPtr<IMF2DBuffer> b2d;
    if (SUCCEEDED(buf.As(&b2d)) && SUCCEEDED(b2d->Lock2D(&p, &pitchY))) {
        locked2d = true;
    } else {
        p = nullptr;
        if (FAILED(buf->Lock(&p, nullptr, nullptr)) || !p) return;
    }

    int w = 0, h = 0;
    if (!CurrentFrameSize(&w, &h) || w <= 0 || h <= 0) {
        if (locked2d) b2d->Unlock2D(); else buf->Unlock();
        return;
    }
    int strideY = locked2d ? (int)pitchY : w;
    const uint8_t* py  = p;
    const uint8_t* puv = p + (size_t)strideY * h;

    // 拷贝为 NV12Buffer（渲染器 NV12 纹理路径直接消费；后续零 CPU 转换直上 GPU）
    auto nv12 = webrtc::NV12Buffer::Create(w, h);
    for (int r = 0; r < h; ++r)
        memcpy(nv12->MutableDataY() + (size_t)r * nv12->StrideY(),
               py + (size_t)r * strideY, (size_t)w);
    for (int r = 0; r < h / 2; ++r)
        memcpy(nv12->MutableDataUV() + (size_t)r * nv12->StrideUV(),
               puv + (size_t)r * strideY, (size_t)w);

    if (locked2d) b2d->Unlock2D(); else buf->Unlock();

    LONGLONG t100 = 0;
    s->GetSampleTime(&t100);
    int64_t tsUs = t100 / 10;  // 100ns → us
    if (tsUs > lastTsUs_) lastTsUs_ = tsUs;

    auto frame = webrtc::VideoFrame::Builder()
                     .set_video_frame_buffer(nv12)
                     .set_timestamp_us(lastTsUs_)
                     .build();
    callback_->Decoded(frame);  // m140：Decoded(VideoFrame&) 非常量引用，传左值
}

}  // namespace vm::hw
