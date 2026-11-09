// =============================================================================
// 共享 D3D11 设备（懒创建，单例）
//   - 硬件优先（任意厂商），失败回退 WARP 软件光栅化（虚拟机/无 GPU 也能跑）
//   - 当前供 D3D11 渲染器使用；MF 解码器的 DXVA 零拷贝路径（IMFDXGIDeviceManager
//     共享纹理直达渲染）为预留扩展：届时解码/渲染共用本设备即可打通
// =============================================================================
#pragma once

#include <d3d11.h>
#include <mfidl.h>
#include <wrl/client.h>

namespace vm::hw {

using Microsoft::WRL::ComPtr;

// 进程级共享设备（首次调用创建；线程安全）
ComPtr<ID3D11Device> SharedD3D11Device();

// 设备的立即上下文（当前约定：仅渲染线程使用）
ComPtr<ID3D11DeviceContext> SharedD3D11Context();

// 当前设备是否为 WARP 软件回退（用于日志提示）
bool IsWarpDevice();

// ===========================================================================
// MF/DXVA 专用设备（与渲染器共享设备分离，避免影响已调好的渲染路径）
//   带 D3D11_CREATE_DEVICE_VIDEO_SUPPORT + ID3D10Multithread 保护，
//   是 MFT_MESSAGE_SET_D3D_MANAGER 的前提（NVENC/NVDEC GPU 路径）。
//   创建失败返回空 → 调用方自动走系统内存路径，不影响功能。
// ===========================================================================
ComPtr<ID3D11Device> VideoD3D11Device();
ComPtr<IMFDXGIDeviceManager> VideoDXGIDeviceManager();

}  // namespace vm::hw
