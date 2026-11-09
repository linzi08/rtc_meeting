# libmediasoupclient 视频编解码工厂注入补丁（适用本机 m140 构建）

## 目的

libmediasoupclient 在 `src/PeerConnection.cpp` 的 PeerConnection 构造函数里调用
`webrtc::CreatePeerConnectionFactory(...)`，第 7/8 个参数写死为内建软编工厂
（VP8/VP9/OpenH264/AV1 模板）。本补丁把这两个参数替换为客户端注入的组合工厂
（**MF 硬件 H264 优先 + 内建软编兜底**）——这是硬编解生效的唯一改动点。

SVC **不需要**补丁：客户端经 `Produce` 的 `encodings` 参数注入 `scalability_mode`。

全部是**新增/替换表达式**，不改任何既有逻辑；不打补丁时客户端用
`-DVM_HW_INJECT=OFF` 构建（自动回退内建软编），功能完全不受影响。

## 修改步骤（共 2 处，约 25 行）

### ① `include/PeerConnection.hpp` —— `mediasoupclient::PeerConnection` 类的 public 区新增

```cpp
// ==== video_meeting 硬编解码注入（begin）====
// 注入视频编解码工厂（在创建任何 transport 之前调用一次）
static void SetVideoCodecFactories(
    std::unique_ptr<webrtc::VideoEncoderFactory> encoderFactory,
    std::unique_ptr<webrtc::VideoDecoderFactory> decoderFactory);
// ==== video_meeting 硬编解码注入（end）====
```

（头文件需能见到 `webrtc::VideoEncoderFactory`/`VideoDecoderFactory`；
该头已包含 `api/peer_connection_interface.h`，间接可见，缺则补
`#include <api/video_codecs/video_encoder_factory.h>` 等两行。）

### ② `src/PeerConnection.cpp` —— 两步：

**②-a** 文件头部（include 之后、匿名命名空间或任意位置）新增静态变量与实现：

```cpp
// ==== video_meeting 硬编解码注入（begin）====
namespace {
// 客户端注入的工厂（进程级；首次创建 PCF 时消费）
std::unique_ptr<webrtc::VideoEncoderFactory> g_vmEncoderFactory;
std::unique_ptr<webrtc::VideoDecoderFactory> g_vmDecoderFactory;
}  // namespace

/* static */
void PeerConnection::SetVideoCodecFactories(
    std::unique_ptr<webrtc::VideoEncoderFactory> encoderFactory,
    std::unique_ptr<webrtc::VideoDecoderFactory> decoderFactory) {
  g_vmEncoderFactory = std::move(encoderFactory);
  g_vmDecoderFactory = std::move(decoderFactory);
}
// ==== video_meeting 硬编解码注入（end）====
```

**②-b** 在 PeerConnection 构造函数里找到这一段（约第 100 行）：

```cpp
this->peerConnectionFactory = webrtc::CreatePeerConnectionFactory(
    this->networkThread.get(),
    this->workerThread.get(),
    this->signalingThread.get(),
    nullptr /*default_adm*/,
    webrtc::CreateBuiltinAudioEncoderFactory(),
    webrtc::CreateBuiltinAudioDecoderFactory(),
    std::make_unique<webrtc::VideoEncoderFactoryTemplate<...>>(),   // ← 第 7 参
    std::make_unique<webrtc::VideoDecoderFactoryTemplate<...>>());  // ← 第 8 参
```

把第 7/8 两个参数替换为三元表达式（注入了就用注入的，否则保持原样）：

```cpp
    // ==== video_meeting：优先使用注入的硬编解码组合工厂（未注入则走原内建）====
    g_vmEncoderFactory
      ? std::move(g_vmEncoderFactory)
      : std::make_unique<webrtc::VideoEncoderFactoryTemplate<
          webrtc::LibvpxVp8EncoderTemplateAdapter,
          webrtc::LibvpxVp9EncoderTemplateAdapter,
          webrtc::OpenH264EncoderTemplateAdapter,
          webrtc::LibaomAv1EncoderTemplateAdapter>>(),
    g_vmDecoderFactory
      ? std::move(g_vmDecoderFactory)
      : std::make_unique<webrtc::VideoDecoderFactoryTemplate<
          webrtc::LibvpxVp8DecoderTemplateAdapter,
          webrtc::LibvpxVp9DecoderTemplateAdapter,
          webrtc::OpenH264DecoderTemplateAdapter,
          webrtc::Dav1dDecoderTemplateAdapter>>());
```

（三元里保留的内建模板参数照抄你文件里原有的即可——上面只是示例，
以你 `src/PeerConnection.cpp` 第 107–115 行的实际适配器列表为准。）

## 重编 libmediasoupclient

按你最初编译的同一套命令重新构建（Ninja）：

```bash
cd D:\work\work_code\libmediasoupclient\build
cmake --build .          # 或 ninja mediasoupclient
# 产物：build\mediasoupclient.lib
```

## 客户端如何使用 / 不使用补丁

| 库状态 | 客户端构建 | 行为 |
|---|---|---|
| 已打补丁 | 默认（`VM_HW_INJECT=ON`） | 编码：MF 硬件 H264（三厂）优先，无硬件回退内建软编（含 OpenH264-H264）；解码：MF 硬件 → 微软软解 MFT → 内建软解三级兜底；SVC 可用 |
| 未打补丁 | `-DVM_HW_INJECT=OFF` | 链接不引用补丁符号；视频=内建软编；信令/渲染/屏幕共享/SVC/重连/TURN 不受影响 |

硬/软编的每次决策都有日志（`[codec][编码] 硬件 H264 可用(...) → 优先级: MF硬编 > 内建软编兜底` 等），
见 `client/src/media/hw_codecs/mf_encoder_factory.cpp`、`mf_video_decoder.cpp`、
`client/src/media/media_engine.cpp` 的【硬编优先/软编兜底】注释块。

## 验证

1. 启动客户端，日志应出现：
   - `[codec][编码] 硬件 H264 可用（NVIDIA H.264 Encoder MFT...）→ 优先级: MF硬编 > 内建软编兜底`
   - `[codec][解码] 使用硬件解码 MFT: ...（DXVA/D3D11）`
2. 任务管理器 → GPU →「Video Encode / Video Decode」有占用（硬编解生效的直接证据）。
3. 对照：`--no-hw` 跑一次，CPU 占用明显上升。
