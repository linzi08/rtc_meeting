#include "audio_capture.h"

#include "../util/log.h"

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <chrono>
#include <cmath>
#include <vector>

namespace vm::media {

static const char* TAG = "audio";

bool WasapiMicCapture::Start(PcmAudioSource* sink) {
    Stop();
    sink_    = sink;
    running_ = true;
    thread_  = std::thread([this] { Run(); });
    return true;
}

void WasapiMicCapture::Stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    sink_ = nullptr;
}

// ---------------------------------------------------------------------------
// 采集线程：
//   WASAPI 共享模式 → 默认麦克风 → 混合格式（通常 32bit float/48k，声道数不定）
//   → 转 16bit/48k/单声道 → PcmAudioSource::PushPcm → 编码发送通道
//   （共享模式允许多客户端同时采集 → 同机多开不受影响）
// ---------------------------------------------------------------------------
void WasapiMicCapture::Run() {
    // WASAPI 需要 COM（MTA）
    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool    comOk = SUCCEEDED(hrCom) || hrCom == RPC_E_CHANGED_MODE;

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice*           device     = nullptr;
    IAudioClient*        client     = nullptr;
    IAudioCaptureClient* capture    = nullptr;
    WAVEFORMATEX*        wfx        = nullptr;
    UINT32               bufFrames  = 0;
    bool                 started    = false;

    do {
        if (!comOk) break;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator), (void**)&enumerator)) ||
            !enumerator)
            break;
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device)) || !device)
            break;
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                    (void**)&client)) ||
            !client)
            break;
        if (FAILED(client->GetMixFormat(&wfx)) || !wfx) break;

        // 共享模式：缓冲 100ms，10ms 轮询拉取（简单可靠，不用事件回调）
        if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000 /*hns=100ms*/, 0, wfx,
                                      nullptr)) ||
            FAILED(client->GetBufferSize(&bufFrames)) ||
            FAILED(client->GetService(__uuidof(IAudioCaptureClient), (void**)&capture)) ||
            FAILED(client->Start()))
            break;
        started = true;
    } while (false);

    if (!started) {
        LOGT(TAG, "麦克风初始化失败（--no-audio 可关闭音频）");
        goto cleanup;
    }

    {
        LOGT(TAG, "麦克风采集已启动 (共享模式, mix=%dHz/%dch/%dbits)", wfx->nSamplesPerSec,
             wfx->nChannels, wfx->wBitsPerSample);

        const int  srcRate     = (int)wfx->nSamplesPerSec;
        const int  srcChannels = (int)wfx->nChannels;
        const bool isFloat     = (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                              ((WAVEFORMATEXTENSIBLE*)wfx)->SubFormat.Data1 == 3) ||
                             wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        std::vector<int16_t> mono;
        std::vector<int16_t> resampled;
        double frac = 0.0;  // 帧率≠48k 时的线性重采样游标

        while (running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            UINT32 pad = 0;
            if (FAILED(client->GetCurrentPadding(&pad))) break;
            UINT32 avail = bufFrames > pad ? bufFrames - pad : 0;
            if (avail == 0) continue;

            BYTE*  data = nullptr;
            UINT32 got  = 0;
            DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&data, &got, &flags, nullptr, nullptr)) || !got) {
                if (data) capture->ReleaseBuffer(0);
                continue;
            }
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                capture->ReleaseBuffer(got);
                continue;
            }

            // ① 源格式 → 16bit 单声道（声道取平均；float 或 16bit 整型两种混格式）
            mono.clear();
            mono.reserve(got);
            if (isFloat) {
                const float* p = (const float*)data;
                for (UINT32 i = 0; i < got; ++i) {
                    double s = 0;
                    for (int c = 0; c < srcChannels; ++c) s += p[i * srcChannels + c];
                    s = s / srcChannels * 32767.0;
                    if (s > 32767) s = 32767;
                    if (s < -32768) s = -32768;
                    mono.push_back((int16_t)s);
                }
            } else {
                const int16_t* p = (const int16_t*)data;
                for (UINT32 i = 0; i < got; ++i) {
                    int32_t s = 0;
                    for (int c = 0; c < srcChannels; ++c) s += p[i * srcChannels + c];
                    mono.push_back((int16_t)(s / srcChannels));
                }
            }
            capture->ReleaseBuffer(got);

            // ② 帧率 ≠ 48k 时线性重采样到 48k（绝大多数设备原生 48k，此处仅保险）
            if (srcRate == 48000) {
                if (!mono.empty()) sink_->PushPcm(mono.data(), mono.size(), 48000, 1);
            } else {
                resampled.clear();
                double step = (double)srcRate / 48000.0;
                while ((size_t)(frac + 1.0) < mono.size()) {
                    size_t i0 = (size_t)frac;
                    double t = frac - i0;
                    resampled.push_back((int16_t)(mono[i0] * (1 - t) + mono[i0 + 1] * t));
                    frac += step;
                }
                frac -= mono.size();
                if (!resampled.empty())
                    sink_->PushPcm(resampled.data(), resampled.size(), 48000, 1);
            }
        }

        client->Stop();
    }

cleanup:
    if (capture) capture->Release();
    if (client) client->Release();
    if (wfx) CoTaskMemFree(wfx);
    if (device) device->Release();
    if (enumerator) enumerator->Release();
    if (comOk && hrCom != RPC_E_CHANGED_MODE) CoUninitialize();
}

}  // namespace vm::media
