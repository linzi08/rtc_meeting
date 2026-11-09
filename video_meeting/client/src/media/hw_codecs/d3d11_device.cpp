#include "d3d11_device.h"

#include "../../util/log.h"

#include <d3d10.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mutex>

namespace vm::hw {

namespace {
struct SharedState {
    ComPtr<ID3D11Device>        device;
    ComPtr<ID3D11DeviceContext> context;
    bool                        warp = false;
    bool                        inited = false;
};
SharedState& State() {
    static SharedState s;
    return s;
}

void CreateDevice() {
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    auto& s = State();

    // 1) 硬件设备（任何厂商：Intel / NVIDIA / AMD）
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, &s.device,
                                   nullptr, &s.context);
    if (FAILED(hr)) {
        // 2) WARP 软件光栅化兜底（功能不变，性能略降；虚拟机场景常走这里）
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                               ARRAYSIZE(levels), D3D11_SDK_VERSION, &s.device,
                               nullptr, &s.context);
        s.warp = SUCCEEDED(hr);
        LOGT("render", "D3D11 硬件设备创建失败(0x%08lX)，回退 WARP 软件光栅化", hr);
    }

    DXGI_ADAPTER_DESC desc = {};
    ComPtr<IDXGIDevice>  dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    if (SUCCEEDED(s.device.As(&dxgiDevice)) &&
        SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetDesc(&desc))) {
        char name[128] = {0};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
        LOGT("render", "D3D11 设备: %s%s", name, s.warp ? " (WARP)" : "");
    }
    s.inited = true;
}
}  // namespace

ComPtr<ID3D11Device> SharedD3D11Device() {
    static std::once_flag once;
    std::call_once(once, CreateDevice);
    return State().device;
}

ComPtr<ID3D11DeviceContext> SharedD3D11Context() {
    SharedD3D11Device();  // 确保已创建
    return State().context;
}

bool IsWarpDevice() {
    SharedD3D11Device();
    return State().warp;
}

// ===========================================================================
// MF 专用设备：独立于渲染器共享设备，避免 VIDEO_SUPPORT 影响渲染路径。
// 必须带 D3D11_CREATE_DEVICE_VIDEO_SUPPORT（DXVA/MFT 设备管理器硬性要求），
// 且必须开 ID3D10Multithread（MFT 会在自己的工作线程调用该设备）。
// ===========================================================================
namespace {
struct VideoState {
    ComPtr<ID3D11Device>         device;
    ComPtr<IMFDXGIDeviceManager> manager;
};
 
VideoState& VState() {
    static VideoState s;
    return s;
}
 
void CreateVideoDevice() {
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };
    auto& s = VState();
 
    // 注意：WARP 不支持 VIDEO_SUPPORT，故只试硬件驱动（无 GPU 时自然失败）
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &s.device, nullptr, nullptr);
    if (FAILED(hr)) {
        LOGT("codec", "MF 专用 D3D11 设备创建失败(0x%08lX) → 硬编解走系统内存路径", hr);
        return;
    }
 
    // MF 强制要求：设备开启多线程保护
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(s.device.As(&mt))) mt->SetMultithreadProtected(TRUE);
 
    // 设备管理器（MFT_MESSAGE_SET_D3D_MANAGER 的载体）
    UINT resetToken = 0;
    if (FAILED(MFCreateDXGIDeviceManager(&resetToken, &s.manager))) {
        s.manager.Reset();
        return;
    }
    if (FAILED(s.manager->ResetDevice(s.device.Get(), resetToken))) {
        s.manager.Reset();
        return;
    }
 
    DXGI_ADAPTER_DESC desc = {};
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    if (SUCCEEDED(s.device.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetDesc(&desc))) {
        char name[128] = {0};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
        LOGT("codec", "MF 专用 D3D11 设备就绪: %s（NVENC/NVDEC GPU 路径可用）", name);
    }
}
}  // namespace
 
ComPtr<ID3D11Device> VideoD3D11Device() {
    static std::once_flag once;
    std::call_once(once, CreateVideoDevice);
    return VState().device;
}
 
ComPtr<IMFDXGIDeviceManager> VideoDXGIDeviceManager() {
    VideoD3D11Device();  // 确保已创建
    return VState().manager;
}

}  // namespace vm::hw
