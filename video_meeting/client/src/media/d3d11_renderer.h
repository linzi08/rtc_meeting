// =============================================================================
// D3D11 低延迟零拷贝渲染器（设计文档 4.2）
//
// 数据通路：
//   webrtc 解码线程 OnFrame → latest-wins 信箱（无锁指针交换 + 丢旧帧，
//   不排队、不积压 → 渲染延迟恒定 ≤1 帧）
//     → 渲染线程（独占 D3D11 设备；每窗口一个 FLIP 交换链）
//        ① Map 动态纹理逐平面 memcpy（全链路唯一一次拷贝：GPU 上传）
//        ② 像素着色器 YUV→RGB（BT.601 limited）+ 视口 letterbox 等比缩放
//        ③ Present（默认 vsync；--allow-tearing 时 ALLOW_TEARING 跳过 vsync）
//
// 输入格式：I420（三张 R8 纹理）与 NV12（Y=R8 + UV=R8G8，MF 硬解直出格式）。
// 窗口管理：每路流一个顶层窗口，级联摆放；窗口创建/销毁均在渲染线程执行。
// =============================================================================
#pragma once

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <api/video/video_frame.h>
#include <api/video/video_sink_interface.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vm::render {

// 每路流的内部状态（渲染线程持有；跨线程仅信箱字段可碰）
struct Stream {
    // —— 窗口/交换链/纹理：渲染线程专属 ——
    HWND hwnd = nullptr;
    Microsoft::WRL::ComPtr<IDXGISwapChain1>          swapchain;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          texY, texU, texV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srvY, srvU, srvV;
    bool nv12Mode = false;  // true: Y+UV 两张；false: Y/U/V 三张
    int  texW = 0, texH = 0;
    bool resizePending = false;

    // —— 跨线程信箱：解码线程写，渲染线程读（latest-wins）——
    std::mutex m;
    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> latest;
    bool dirty = false;

    std::string       title;   // 含 peer 名/kind
    std::string       suffix;  // 如 " [paused]"
    std::atomic<bool> closed{false};
};

// 对外句柄：实现 webrtc::VideoSinkInterface，AddSink 到 Consumer/本地轨道
class VideoWindow : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    explicit VideoWindow(std::shared_ptr<Stream> s) : stream_(std::move(s)) {}
    ~VideoWindow() override { Close(); }

    // 解码线程调用：投递信箱（只做指针交换，绝不阻塞解码线程）
    void OnFrame(const webrtc::VideoFrame& frame) override;

    // 更新标题后缀（如 ⏸ 暂停标记）——线程安全
    void SetSuffix(const std::string& suffix);

    // 请求销毁窗口（幂等，线程安全；实际销毁在渲染线程执行）
    void Close();

private:
    std::shared_ptr<Stream> stream_;
};

class RendererManager {
public:
    static RendererManager& Instance();

    // 启动渲染线程 + 初始化 D3D（main 里调用一次）
    void Start(bool allowTearing);
    void Shutdown();

    // 创建一路视频窗口（阻塞至窗口建好；初始尺寸按流分辨率、级联摆放）
    std::shared_ptr<VideoWindow> CreateStream(const std::string& title, int w, int h);

    // 解码线程投帧后唤醒渲染线程（VideoWindow::OnFrame 调用）
    void WakeRenderThread();
    // 跨线程投递渲染线程任务（窗口创建/销毁/改名必须在渲染线程；VideoWindow 调用）
    void PostToRenderThread(std::function<void()>);

private:
    void RenderThreadMain();
    bool InitD3D();                                 // 设备/着色器/采样器
    bool EnsureSwapchain(Stream& s);
    bool EnsureTextures(Stream& s, bool nv12, int w, int h);
    void RenderStream(Stream& s,
                      webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buf);

    std::thread      renderThread_;
    std::atomic<bool> running_{false};
    HANDLE wakeEvent_ = nullptr;  // 新帧/新任务唤醒渲染线程

    // 渲染线程任务队列（窗口创建/销毁/改名必须在渲染线程）
    std::mutex taskMutex_;
    std::deque<std::function<void()>> tasks_;

    // D3D11 资源（渲染线程专属；设备来自 hw::SharedD3D11Device）
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader>  vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>   psI420_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>   psNV12_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>  sampler_;
    bool allowTearing_      = false;  // --allow-tearing 请求
    bool tearingSupported_  = false;  // 系统是否实际支持

    std::vector<std::shared_ptr<Stream>> streams_;  // 仅渲染线程访问
    int cascadeIndex_ = 0;                          // 级联摆放计数
};

}  // namespace vm::render