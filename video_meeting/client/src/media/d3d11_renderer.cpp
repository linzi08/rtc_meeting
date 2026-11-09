// =============================================================================
// D3D11 低延迟零拷贝渲染器 —— 实现（设计文档 4.2）
// 数据通路：解码线程 OnFrame → latest-wins 信箱 → 渲染线程上传纹理 + GPU 上色 + FLIP Present
// =============================================================================
#include "d3d11_renderer.h"

#include "../util/log.h"
#include "hw_codecs/d3d11_device.h"

#include <api/video/i420_buffer.h>
#include <api/video/nv12_buffer.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <future>

namespace vm::render {

static const char* TAG = "render";

// ---------------------------------------------------------------------------
// 着色器
//   顶点：SV_VertexID 生成全屏三角形（无顶点缓冲，零 CPU 准备开销）
//   像素：YUV(BT.601 limited)→RGB，全部在 GPU 完成（CPU 零像素转换）
//   I420 = Y/U/V 三张 R8 纹理；NV12 = Y(R8) + UV(R8G8) 两张（MF 硬解直出格式）
// ---------------------------------------------------------------------------
static const char* kVsSrc = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD; };
VSOut VS(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}
)";


static const char* kPsI420Src = R"(
Texture2D<float4> texY : register(t0);
Texture2D<float4> texU : register(t1);
Texture2D<float4> texV : register(t2);
SamplerState samp : register(s0);
float4 PS(float2 uv : TEXCOORD) : SV_Target {
    float y = texY.Sample(samp, uv).r;
    // float u = texU.Sample(samp, uv).r - 0.5;
    // float v = texV.Sample(samp, uv).r - 0.5;
    // float r = 1.164383 * (y - 0.0625) + 1.596027 * v;
    // float g = 1.164383 * (y - 0.0625) - 0.391762 * u - 0.812968 * v;
    // float b = 1.164383 * (y - 0.0625) + 2.017232 * u;
    // return float4(r, g, b, 1.0);
    return float4(y, y, y, 1.0);
}
)";

static const char* kPsNV12Src = R"(
Texture2D<float4> texY  : register(t0);
Texture2D<float4> texUV : register(t1);   // R8G8: r=U, g=V
SamplerState samp : register(s0);
float4 PS(float2 uv : TEXCOORD) : SV_Target {
    float y  = texY.Sample(samp, uv).r;
    float2 q = texUV.Sample(samp, uv).rg - 0.5;
    float r = 1.164383 * (y - 0.0625) + 1.596027 * q.y;
    float g = 1.164383 * (y - 0.0625) - 0.391762 * q.x - 0.812968 * q.y;
    float b = 1.164383 * (y - 0.0625) + 2.017232 * q.x;
    return float4(r, g, b, 1.0);
}
)";

// ---------------------------------------------------------------------------
// 窗口消息：关闭（点 X）/ 缩放 / 不擦背景（防闪烁）
// ---------------------------------------------------------------------------
namespace {

LRESULT CALLBACK StreamWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Stream* s = (Stream*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_SIZE:
            if (s) s->resizePending = true;  // 交换链在下一次渲染前 Resize
            return 0;
        case WM_DESTROY:
            if (s) s->closed.store(true);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

bool CompileShader(const char* src, const char* entry, const char* target,
                   Microsoft::WRL::ComPtr<ID3DBlob>* out) {
    Microsoft::WRL::ComPtr<ID3DBlob> err;
    HRESULT hr = D3DCompile(src, (SIZE_T)strlen(src), nullptr, nullptr, nullptr, entry, target, 0,
                            0, out->ReleaseAndGetAddressOf(), err.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        if (err) LOGT(TAG, "着色器编译失败: %s", (const char*)err->GetBufferPointer());
        return false;
    }
    return true;
}

}  // namespace

// ===========================================================================
// VideoWindow（对外句柄，跨线程使用）
// ===========================================================================
void VideoWindow::OnFrame(const webrtc::VideoFrame& frame) {
    try {
    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buf = frame.video_frame_buffer();
    if (!buf) return;
    static int frameCount = 0;
    if (frameCount++ % 30 == 0) {
        int fw = buf->width(), fh = buf->height();
        LOGT(TAG, "[诊断] 帧格式=%d, 尺寸=%dx%d", (int)buf->type(), fw, fh);
        // 帧 = 缓冲区的实际宽高。注意：这里同时打印 buf 边长，用于
        // 与各平面 Stride 对比，判断是否出现"尺寸/Stride 不一致"。
        if (buf->type() == webrtc::VideoFrameBuffer::Type::kI420) {
            auto* i420 = buf->GetI420();
            if (i420) {
                // 去决定性自检：dump 首帧 Y 平面到 frameY.pgm（判别数据本身是否正常）
                static bool dumpedY = false;
                if (!dumpedY) {
                    dumpedY = true;
                    FILE* f = fopen("frameY.pgm", "wb");
                    if (f) {
                        char hdr[64];
                        std::snprintf(hdr, sizeof(hdr), "P5\n%d %d\n255\n", fw, fh);
                        fwrite(hdr, 1, std::strlen(hdr), f);
                        for (int y = 0; y < fh; ++y)
                            fwrite(i420->DataY() + (size_t)y * i420->StrideY(), 1, fw, f);
                        fclose(f);
                        LOGT(TAG, "[诊断] 已 dump 首帧 Y => frameY.pgm (%dx%d)", fw, fh);
                    }
                }
                
                int sy = i420->StrideY(), su = i420->StrideU(), sv = i420->StrideV();
                LOGT(TAG, "[诊断] I420 帧WxH=%dx%d, Y=stride=%d(比宽x%.1f), U=%d(比半宽x%.1f), V=%d(比半宽x%.1f)",
                     fw, fh, sy, (float)sy / fw, su, (float)su / ((fw + 1) / 2),
                     sv, (float)sv / ((fw + 1) / 2));
                LOGT(TAG, "[诊断] 平面宽: Y=%d, U/V半宽=%d ; 平面高: Y=%d, U/V半高=%d",
                     fw, (fw + 1) / 2, fh, (fh + 1) / 2);
            }
        } else if (buf->type() == webrtc::VideoFrameBuffer::Type::kNV12) {
            auto* nv12 = buf->GetNV12();
            if (nv12) {
                int sy = nv12->StrideY(), suv = nv12->StrideUV();
                LOGT(TAG, "[诊断] NV12 帧WxH=%dx%d, Y=stride=%d(比宽x%.1f), UV-stride=%d(比宽x%.1f)",
                     fw, fh, sy, (float)sy / fw, suv, (float)suv / fw);
            }
        }
    }
    if (buf->type() == webrtc::VideoFrameBuffer::Type::kNative) {
        LOGT(TAG, "[诊断] 发现 kNative 格式，强制转 I420");
        buf = buf->ToI420();
        if (!buf) return;

        // 转换后宽高可能与原始不一致，记录下来便于核对
        static int cvtCount = 0;
        if (cvtCount++ < 6)
            LOGT(TAG, "[诊断] kNative 转换后 => 尺寸=%dx%d", buf->width(), buf->height());
    }
    // 【诊断点 2】打印转换后的实际 Stride
    // if (buf->type() == webrtc::VideoFrameBuffer::Type::kI420) {
    //     auto* i420 = buf->GetI420();
    //     if (i420) {
    //         LOGT(TAG, "[诊断] I420 Y-Stride=%d, U-Stride=%d, V-Stride=%d", 
    //                 i420->StrideY(), i420->StrideU(), i420->StrideV());
    //     }
    // } else if (buf->type() == webrtc::VideoFrameBuffer::Type::kNV12) {
    //     auto* nv12 = buf->GetNV12();
    //     if (nv12) {
    //         LOGT(TAG, "[诊断] NV12 Y-Stride=%d, UV-Stride=%d", 
    //                 nv12->StrideY(), nv12->StrideUV());
    //     }
    // }
    {
        std::lock_guard<std::mutex> lock(stream_->m);
        stream_->latest = buf;
        stream_->dirty  = true;
    }
    RendererManager::Instance().WakeRenderThread();
    } catch (const std::exception& e) {
        LOGT(TAG, "OnFrame 异常: %s", e.what());
    } catch (...) {
        LOGT(TAG, "OnFrame 未知异常");
    }
}

void VideoWindow::SetSuffix(const std::string& suffix) {
    auto weak = std::weak_ptr<Stream>(stream_);
    RendererManager::Instance().PostToRenderThread([weak, suffix]() {
        if (auto s = weak.lock()) {
            s->suffix = suffix;
            if (s->hwnd) SetWindowTextA(s->hwnd, (s->title + s->suffix).c_str());
        }
    });
}

void VideoWindow::Close() {
    auto s = stream_;
    RendererManager::Instance().PostToRenderThread([s]() { if (s) s->closed.store(true); });
}

// ===========================================================================
// RendererManager
// ===========================================================================
RendererManager& RendererManager::Instance() {
    static RendererManager inst;
    return inst;
}

void RendererManager::Start(bool allowTearing) {
    if (running_.exchange(true)) return;
    allowTearing_     = allowTearing;
    tearingSupported_ = false;
    wakeEvent_        = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    renderThread_     = std::thread([this] { RenderThreadMain(); });
    LOGT(TAG, "渲染器启动 (tearing 模式=%s)", allowTearing ? "请求开启" : "关闭");
}

void RendererManager::Shutdown() {
    if (!running_.exchange(false)) return;
    LOGT(TAG, "Shutdown: 渲染器开始关闭...");
    if (wakeEvent_) SetEvent(wakeEvent_);
    if (renderThread_.joinable()) renderThread_.join();
    if (wakeEvent_) {
        CloseHandle(wakeEvent_);
        wakeEvent_ = nullptr;
    }
    context_.Reset();
    vs_.Reset();
    psI420_.Reset();
    psNV12_.Reset();
    sampler_.Reset();
    LOGT(TAG, "Shutdown: 渲染器已关闭");
}

void RendererManager::PostToRenderThread(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(taskMutex_);
        tasks_.push_back(std::move(task));
    }
    if (wakeEvent_) SetEvent(wakeEvent_);
}

void RendererManager::WakeRenderThread() {
    if (wakeEvent_) SetEvent(wakeEvent_);
}

std::shared_ptr<VideoWindow> RendererManager::CreateStream(const std::string& title, int w, int h) {
    LOGT(TAG, "CreateStream: title=%s w=%d h=%d", title.c_str(), w, h);
    auto s = std::make_shared<Stream>();
    s->title = title;

    // 窗口必须建在渲染线程（消息泵亲和）——投任务并等待完成
    std::promise<bool> done;
    auto  fut  = done.get_future();
    auto  weak = std::weak_ptr<Stream>(s);
    PostToRenderThread([this, weak, w, h, &done]() {
        auto s = weak.lock();
        if (!s) {
            LOGT(TAG, "CreateStream: Stream 已释放，跳过");
            done.set_value(false);
            return;
        }
        // 初始尺寸：按流分辨率，宽封顶 640；级联摆放
        int ww = w > 0 ? w : 640, hh = h > 0 ? h : 360;
        if (ww > 640) {
            hh = (int)((int64_t)hh * 640 / ww);
            ww = 640;
        }
        int x = 30 + (cascadeIndex_ % 8) * 46;
        int y = 30 + (cascadeIndex_ % 8) * 46;
        cascadeIndex_++;

        RECT rc = {0, 0, ww, hh};
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        s->hwnd = CreateWindowExW(0, L"VMStreamWnd", L"", WS_OVERLAPPEDWINDOW, x, y,
                                  rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
        if (!s->hwnd) {
            LOGT(TAG, "CreateStream: CreateWindowExW 失败");
            done.set_value(false);
            return;
        }
        SetWindowLongPtrW(s->hwnd, GWLP_USERDATA, (LONG_PTR)s.get());
        SetWindowTextA(s->hwnd, (s->title + s->suffix).c_str());
        ShowWindow(s->hwnd, SW_SHOW);
        if (!EnsureSwapchain(*s)) {
            LOGT(TAG, "CreateStream: EnsureSwapchain 失败");
        }
        streams_.push_back(s);  // streams_ 仅渲染线程访问
        LOGT(TAG, "CreateStream: 窗口创建成功 hwnd=%p", (void*)s->hwnd);
        done.set_value(true);
    });

    bool ok = fut.get();
    if (!ok) {
        LOGT(TAG, "CreateStream: 失败，返回 nullptr");
    }
    return ok ? std::make_shared<VideoWindow>(s) : nullptr;
}

// ---------------------------------------------------------------------------
// 渲染线程主循环：窗口消息泵 + 跨线程任务 + 脏流渲染
// ---------------------------------------------------------------------------
void RendererManager::RenderThreadMain() {
    WNDCLASSW wc = {};
    wc.lpfnWndProc   = StreamWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"VMStreamWnd";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);  // 宏自动匹配 A/W
    RegisterClassW(&wc);

    if (!InitD3D()) {
        LOGT(TAG, "渲染线程: InitD3D 失败，渲染将不可用");
    } else {
        LOGT(TAG, "渲染线程: D3D11 初始化成功");
    }

    while (running_.load()) {
        try {
        // 1) 等"新帧事件"或"窗口消息"，最多 50ms（有新帧立即醒 → 低延迟）
        MsgWaitForMultipleObjects(1, &wakeEvent_, FALSE, 50, QS_ALLINPUT);

        // 2) 执行跨线程任务（建窗/销毁/改标题）
        std::deque<std::function<void()>> tasks;
        {
            std::lock_guard<std::mutex> lock(taskMutex_);
            tasks.swap(tasks_);
        }
        for (auto& t : tasks) {
            try {
                t();
            } catch (const std::exception& e) {
                LOGT(TAG, "渲染线程任务异常: %s", e.what());
            } catch (...) {
                LOGT(TAG, "渲染线程任务未知异常");
            }
        }

        // 3) 消息泵（本线程的所有流窗口）
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // 4) 渲染脏流；清理已关闭的流
        for (auto it = streams_.begin(); it != streams_.end();) {
            Stream& s = **it;
            if (s.closed.load()) {
                if (s.hwnd && IsWindow(s.hwnd)) DestroyWindow(s.hwnd);
                it = streams_.erase(it);
                continue;
            }
            webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buf;
            {
                std::lock_guard<std::mutex> lock(s.m);
                if (s.dirty && s.latest) {
                    buf     = s.latest;
                    s.dirty = false;
                }
            }
            if (buf) {
                try {
                    RenderStream(s, buf);
                } catch (const std::exception& e) {
                    LOGT(TAG, "RenderStream 异常: %s (title=%s)", e.what(), s.title.c_str());
                } catch (...) {
                    LOGT(TAG, "RenderStream 未知异常 (title=%s)", s.title.c_str());
                }
            }
            ++it;
        }
        } catch (const std::exception& e) {
            LOGT(TAG, "渲染线程主循环异常: %s", e.what());
        } catch (...) {
            LOGT(TAG, "渲染线程主循环未知异常");
        }
    }

    for (auto& s : streams_) {
        if (s->hwnd && IsWindow(s->hwnd)) DestroyWindow(s->hwnd);
    }
    streams_.clear();
}

bool RendererManager::InitD3D() {
    auto device = hw::SharedD3D11Device();
    context_    = hw::SharedD3D11Context();
    if (!device || !context_) {
        LOGT(TAG, "InitD3D 失败: D3D11 设备/上下文为空");
        return false;
    }

    Microsoft::WRL::ComPtr<ID3DBlob> vsb, ps1, ps2;
    if (!CompileShader(kVsSrc, "VS", "vs_5_0", &vsb) ||
        !CompileShader(kPsI420Src, "PS", "ps_5_0", &ps1) ||
        !CompileShader(kPsNV12Src, "PS", "ps_5_0", &ps2)) {
        LOGT(TAG, "InitD3D 失败: 着色器编译失败");
        return false;
    }
    device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_);
    device->CreatePixelShader(ps1->GetBufferPointer(), ps1->GetBufferSize(), nullptr, &psI420_);
    device->CreatePixelShader(ps2->GetBufferPointer(), ps2->GetBufferSize(), nullptr, &psNV12_);

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device->CreateSamplerState(&sd, &sampler_);
    LOGT(TAG, "[诊断-采样] 采样器已创建，Filter=POINT, Address=CLAMP");

    // tearing 支持探测（IDXGIFactory5::FEATURE_PRESENT_ALLOW_TEARING）
    if (allowTearing_) {
        Microsoft::WRL::ComPtr<IDXGIDevice>   dxgiDevice;
        Microsoft::WRL::ComPtr<IDXGIAdapter>  adapter;
        Microsoft::WRL::ComPtr<IDXGIFactory5> factory;
        if (SUCCEEDED(device.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
            BOOL allow = FALSE;
            if (SUCCEEDED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                       &allow, sizeof(allow)))) {
                tearingSupported_ = !!allow;
            }
        }
        if (!tearingSupported_) LOGT(TAG, "系统不支持 tearing，回退 vsync 呈现");
    }

    return vs_ && psI420_ && psNV12_ && sampler_;
}

bool RendererManager::EnsureSwapchain(Stream& s) {
    RECT rc;
    GetClientRect(s.hwnd, &rc);
    // LOGT(TAG, "[诊断] client=%dx%d, backbuffer=%ux%u",
    //     rc.right-rc.left, rc.bottom-rc.top,
    //     backDesc.Width, backDesc.Height);
    
    int w = rc.right - rc.left > 8 ? rc.right - rc.left : 320;
    int h = rc.bottom - rc.top > 8 ? rc.bottom - rc.top : 180;

    LOGT(TAG, "[诊断] EnsureSwapchain client=%dx%d", w, h);

    // FLIP 交换链 + 2 缓冲（低延迟标准配置）
    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width       = w;
    scd.Height      = h;
    scd.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc  = {1, 0};
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    auto device = hw::SharedD3D11Device();
    Microsoft::WRL::ComPtr<IDXGIDevice>   dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter>  adapter;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    if (FAILED(device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
        return false;

    HRESULT hr = factory->CreateSwapChainForHwnd(device.Get(), s.hwnd, &scd, nullptr, nullptr,
                                                 &s.swapchain);
    if (FAILED(hr)) {
        LOGT(TAG, "交换链创建失败 0x%08lX", hr);
        return false;
    }

    // MaximumFrameLatency=1：在途帧最多 1 个（Present 后立即提交，不排队）
    Microsoft::WRL::ComPtr<IDXGISwapChain2> sc2;
    if (SUCCEEDED(s.swapchain.As(&sc2))) sc2->SetMaximumFrameLatency(1);
    return true;
}

bool RendererManager::EnsureTextures(Stream& s, bool nv12, int w, int h) {
    if (s.texY && s.texW == w && s.texH == h && s.nv12Mode == nv12) return true;

    auto device = hw::SharedD3D11Device();
    D3D11_TEXTURE2D_DESC td = {};
    td.Width          = w;
    td.Height         = h;
    td.MipLevels      = 1;
    td.ArraySize      = 1;
    td.Format         = DXGI_FORMAT_R8_UNORM;  // Y 平面
    td.SampleDesc     = {1, 0};
//    td.Usage          = D3D11_USAGE_DYNAMIC;   // 动态纹理：Map+WRITE_DISCARD 逐帧上传
    td.Usage          = D3D11_USAGE_DEFAULT;   // 静态纹理：CreateTexture2D 一次创建
    td.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
//    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    td.CPUAccessFlags = 0;

    auto makeR8 = [&](int width, int height, Microsoft::WRL::ComPtr<ID3D11Texture2D>* tex,
                      Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>* srv) {
        D3D11_TEXTURE2D_DESC d = td;
        d.Width  = width;
        d.Height = height;
        if (FAILED(device->CreateTexture2D(&d, nullptr, tex->ReleaseAndGetAddressOf())))
            return false;
        if (FAILED(device->CreateShaderResourceView(tex->Get(), nullptr,
                                                    srv->ReleaseAndGetAddressOf())))
            return false;
        return true;
    };

    s.texY.Reset(); s.texU.Reset(); s.texV.Reset();
    s.srvY.Reset(); s.srvU.Reset(); s.srvV.Reset();

    if (!makeR8(w, h, &s.texY, &s.srvY)) return false;
    if (nv12) {
        // UV 平面：R8G8（半宽半高）—— NV12 路径只有 Y+UV 两张
        D3D11_TEXTURE2D_DESC d = td;
        d.Width  = w / 2;
        d.Height = h / 2;
        d.Format = DXGI_FORMAT_R8G8_UNORM;
        LOGT(TAG, "[诊断-纹理] 创建 NV12 UV 纹理 %ux%u", d.Width, d.Height);
        if (FAILED(device->CreateTexture2D(&d, nullptr, s.texU.ReleaseAndGetAddressOf())))
            return false;
        if (FAILED(device->CreateShaderResourceView(s.texU.Get(), nullptr,
                                                    s.srvU.ReleaseAndGetAddressOf())))
            return false;
        s.texV.Reset(); s.srvV.Reset();
    } else {
        LOGT(TAG, "[诊断-纹理] 创建 I420 U/V 纹理 %ux%u", (w + 1) / 2, (h + 1) / 2);
        if (!makeR8(w / 2, h / 2, &s.texU, &s.srvU)) return false;
        if (!makeR8(w / 2, h / 2, &s.texV, &s.srvV)) return false;
    }
    s.nv12Mode = nv12;
    s.texW     = w;
    s.texH     = h;
    // 【诊断】核对纹理实际宽高 vs 视频帧宽高（高度不一致正是“顶部1/4正常+其余3/4拉伸”的直接指纹）
    D3D11_TEXTURE2D_DESC tdCheck;
    s.texY->GetDesc(&tdCheck);
    LOGT(TAG, "[诊断-纹理宽高] ------------- texY 实际=%ux%u, 视频帧=%dx%d, nv12=%d",
         tdCheck.Width, tdCheck.Height, w, h, (int)nv12);
    return true;
}

static bool UploadPlane(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex,
                        const uint8_t* src, int srcStride, int height, int actualWidthBytes,
                        const char* planeName) {
    // UpdateSubresource 要求传入的 rowPitch 是 srcStride
    // pSrcData 指向原始 CPU 数据，height 是行数
    // D3D11 会自动处理 GPU 纹理 RowPitch 与 srcStride 不一致的情况
    D3D11_BOX box = {0, 0, 0, (UINT)actualWidthBytes, (UINT)height, 1};
    ctx->UpdateSubresource(tex, 0, &box, src, srcStride, 0);

    static int logCount = 0;
    if (logCount++ < 6) {
        LOGT(TAG, "[诊断-上传] %s: 使用 UpdateSubresource, SrcStride=%d, 宽=%d, 高=%d", 
             planeName, srcStride, actualWidthBytes, height);
    }
    return true;
}

void RendererManager::RenderStream(Stream& s,
                                    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buf) {
    if (!s.swapchain && !EnsureSwapchain(s)) {
        LOGT(TAG, "RenderStream: EnsureSwapchain 失败 (title=%s)", s.title.c_str());
        return;
    }
    if (s.resizePending) {
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        HRESULT hr = s.swapchain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
            LOGT(TAG, "ResizeBuffers 失败 0x%08lX (title=%s)", hr, s.title.c_str());
        // } else {
        //     s.resizePending = false;
            return;
        }

     //   s.swapchain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
        s.resizePending = false;
    }

    int w = buf->width(), h = buf->height();
    if (w <= 0 || h <= 0) return;

    bool nv12 = (buf->type() == webrtc::VideoFrameBuffer::Type::kNV12);
    if (!EnsureTextures(s, nv12, w, h)) {
        LOGT(TAG, "RenderStream: EnsureTextures 失败 (title=%s w=%d h=%d nv12=%d)",
             s.title.c_str(), w, h, (int)nv12);
        return;
    }

    if (nv12) {
        auto* nv = buf->GetNV12();
        if (!nv) return;
        UploadPlane(context_.Get(), s.texY.Get(), nv->DataY(), nv->StrideY(), h, w, "NV12-Y");
        UploadPlane(context_.Get(), s.texU.Get(), nv->DataUV(), nv->StrideUV(), (h + 1) / 2, (w + 1) / 2, "NV12-UV");
    } else {
        auto i420 = buf->ToI420();
        if (!i420) return;

        // 渲染线程也 dump 首帧 Y，与 OnFrame 的 frameY.pgm 对照（此行决定问题在数据还是 GPU 呈现）
        static bool dumpedR = false;
        if (!dumpedR) {
            dumpedR = true;
            LOGT(TAG, "[诊断-渲染线程] type=%d w=%d h=%d Y-stride=%d",
                 (int)buf->type(), w, h, i420->StrideY());
            FILE* f = fopen("frameY_render.pgm", "wb");
            if (f) {
                char hdr[64];
                std::snprintf(hdr, sizeof(hdr), "P5\n%d %d\n255\n", w, h);
                fwrite(hdr, 1, std::strlen(hdr), f);
                for (int y = 0; y < h; ++y)
                    fwrite(i420->DataY() + (size_t)y * i420->StrideY(), 1, w, f);
                fclose(f);
                LOGT(TAG, "[诊断] 已 dump 渲染线程 Y => frameY_render.pgm");
            }
        }

        
        UploadPlane(context_.Get(), s.texY.Get(), i420->DataY(), i420->StrideY(), h, w, "I420-Y");
        // 决定性判别：直接从 GPU 读回 texY 纹理内容（区分【上传错】还是【采样/视口错】）
        static bool dumpedTexY = false;
        if (!dumpedTexY) {
            dumpedTexY = true;
            D3D11_TEXTURE2D_DESC tdesc;
            s.texY->GetDesc(&tdesc);
            tdesc.BindFlags      = 0;
            tdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            tdesc.Usage          = D3D11_USAGE_STAGING;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> stg;
            if (SUCCEEDED(hw::SharedD3D11Device()->CreateTexture2D(&tdesc, nullptr, &stg))) {
                context_->CopyResource(stg.Get(), s.texY.Get());
                D3D11_MAPPED_SUBRESOURCE mp;
                if (SUCCEEDED(context_->Map(stg.Get(), 0, D3D11_MAP_READ, 0, &mp))) {
                    FILE* f = fopen("gpu_texY.pgm", "wb");
                    if (f) {
                        char hdr[64];
                        std::snprintf(hdr, sizeof(hdr), "P5\n%u %u\n255\n", tdesc.Width, tdesc.Height);
                        fwrite(hdr, 1, std::strlen(hdr), f);
                        for (UINT y = 0; y < tdesc.Height; ++y)
                            fwrite((const uint8_t*)mp.pData + (size_t)y * mp.RowPitch, 1, tdesc.Width, f);
                        fclose(f);
                        LOGT(TAG, "[诊断] 已 dump GPU texY => gpu_texY.pgm (%ux%u)", tdesc.Width, tdesc.Height);
                    }
                    context_->Unmap(stg.Get(), 0);
                }
            }
        }
        // UploadPlane(context_.Get(), s.texY.Get(), i420->DataY(), i420->StrideY(), h, w, "I420-Y");
        UploadPlane(context_.Get(), s.texU.Get(), i420->DataU(), i420->StrideU(), (h + 1) / 2, (w + 1) / 2, "I420-U");
        UploadPlane(context_.Get(), s.texV.Get(), i420->DataV(), i420->StrideV(), (h + 1) / 2, ( w + 1) / 2, "I420-V");
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
    if (FAILED(s.swapchain->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(hw::SharedD3D11Device()->CreateRenderTargetView(
            back.Get(), nullptr, rtv.ReleaseAndGetAddressOf())))
        return;

    // RECT rc;
    // GetClientRect(s.hwnd, &rc);
    // int bw = rc.right - rc.left, bh = rc.bottom - rc.top;
    // if (bw <= 0 || bh <= 0) return;
    D3D11_TEXTURE2D_DESC backDesc;
    back->GetDesc(&backDesc);   // 以交换链背缓冲物理尺寸为唯一基准，避免与客户区逻辑尺寸错配
    UINT bw = backDesc.Width, bh = backDesc.Height;
    if (bw == 0 || bh == 0) return;

    // 3) letterbox 视口（等比缩放，GPU 完成）
    float scale = std::min((float)bw / w, (float)bh / h);
    int   vw    = (int)(w * scale), vh = (int)(h * scale);
    D3D11_VIEWPORT vp = {(FLOAT)((bw - vw) / 2), (FLOAT)((bh - vh) / 2), (FLOAT)vw, (FLOAT)vh,
                         0.f, 1.f};

    context_->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    // 关键：清空整块背缓冲，避免 FLIP 多缓冲在视口外遗留未定义脏数据导致错位/拉伸
    FLOAT clearColor[4] = {0.f, 0.f, 0.f, 0.f};
    context_->ClearRenderTargetView(rtv.Get(), clearColor);

    context_->RSSetViewports(1, &vp);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->PSSetShader((nv12 ? psNV12_ : psI420_).Get(), nullptr, 0);
    // ID3D11ShaderResourceView* srvs[3] = {s.srvY.Get(), s.srvU.Get(), s.srvV.Get()};
    // context_->PSSetShaderResources(0, 3, srvs);
    // 关键修改：NV12 只绑定 2 个 SRV，I420 绑定 3 个，避免空指针
    if (nv12) {
        ID3D11ShaderResourceView* srvs[2] = {s.srvY.Get(), s.srvU.Get()};
        context_->PSSetShaderResources(0, 2, srvs);
    } else {
        ID3D11ShaderResourceView* srvs[3] = {s.srvY.Get(), s.srvU.Get(), s.srvV.Get()};
        context_->PSSetShaderResources(0, 3, srvs);
    }

    context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
//     D3D11_TEXTURE2D_DESC backDesc;
//     back->GetDesc(&backDesc);
    // LOGT(TAG, "[诊断-渲染] BackBuffer 尺寸: %ux%u", backDesc.Width, backDesc.Height);
    // LOGT(TAG, "[诊断-渲染] Viewport 尺寸: x=%.1f y=%.1f w=%.1f h=%.1f", vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height);
    context_->Draw(3, 0);

    // 决定性判别：读回首帧 backbuffer，确认 GPU 画出的画面本身是否正确
    {
        static bool dumpedBack = false;
        if (!dumpedBack) {
            dumpedBack = true;
            D3D11_TEXTURE2D_DESC bd;
            back->GetDesc(&bd);
            bd.BindFlags      = 0;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            bd.Usage          = D3D11_USAGE_STAGING;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> stg;
            if (SUCCEEDED(hw::SharedD3D11Device()->CreateTexture2D(&bd, nullptr, &stg))) {
                context_->CopyResource(stg.Get(), back.Get());
                D3D11_MAPPED_SUBRESOURCE mp;
                if (SUCCEEDED(context_->Map(stg.Get(), 0, D3D11_MAP_READ, 0, &mp))) {
                    FILE* f = fopen("backbuffer.pgm", "wb");
                    if (f) {
                        char hdr[64];
                        std::snprintf(hdr, sizeof(hdr), "P5\n%u %u\n255\n", bd.Width, bd.Height);
                        fwrite(hdr, 1, std::strlen(hdr), f);
                        for (UINT y = 0; y < bd.Height; ++y) {
                            const uint8_t* row = (const uint8_t*)mp.pData + (size_t)y * mp.RowPitch;
                            for (UINT x = 0; x < bd.Width; ++x) {
                                uint32_t B = row[x * 4], G = row[x * 4 + 1], R = row[x * 4 + 2];
                                uint8_t grey = (uint8_t)((R * 299 + G * 587 + B * 114) / 1000);
                                fwrite(&grey, 1, 1, f);
                            }
                        }
                        fclose(f);
                        LOGT(TAG, "[诊断] 已 dump backbuffer => backbuffer.pgm (%ux%u)",
                             bd.Width, bd.Height);
                    }
                    context_->Unmap(stg.Get(), 0);
                }
            }
        }
    }

    // 4) 呈现：tearing 模式跳过 vsync 等待（最低延迟）；默认 vsync 平滑
    if (allowTearing_ && tearingSupported_) {
        s.swapchain->Present(0, DXGI_PRESENT_ALLOW_TEARING);
    } else {
        s.swapchain->Present(1, 0);
    }
}

}  // namespace vm::render