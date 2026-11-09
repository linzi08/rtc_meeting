# mediasoup 视频会议 — 交互设计与整体架构
（服务端：官方 mediasoup-demo v3，零改动 / 客户端：mediasoup-client C++ 原生库，Windows）

- 版本：v1.0（待评审）
- 日期：2026-11-05
- 前提：`mediasoupclient.lib`（libmediasoupclient + libwebrtc 依赖）已编译；mediasoup-demo 服务端已部署。
- 假设：libmediasoupclient ≥ 3.10。个别回调签名随版本略有差异，编码时以本机头文件为准做少量适配（如能告知具体版本更佳）。
- 硬编解码前提：硬件编解码通过 Media Foundation 编解码器工厂注入，需对 libmediasoupclient 源码打约 30 行补丁后重编（交付补丁 diff 与重编步骤；这是获得三厂硬编解的唯一改动点）。

---

## 1. 总体架构

```
        信令面 (wss, protoo JSON)                           媒体面 (ICE/DTLS/SRTP)
┌──────────────────────┐                              ┌──────────────────────┐
│  原生 C++ 客户端      │                              │  mediasoup-demo 服务端 │
│  ┌────────────────┐  │   wss://IP:4443              │  ┌────────────────┐  │
│  │ ProtooClient   │◄─┼─────────────────────────────►│  │ protoo WS 服务 │  │
│  ├────────────────┤  │   ?room=&peer=               │  │ Room / Peer    │  │
│  │ RoomClient     │  │                              │  ├────────────────┤  │
│  │ (状态机+任务队列)│  │                              │  │ mediasoup      │  │
│  ├────────────────┤  │                              │  │ Worker/Router  │  │
│  │ Device         │  │       SRTP 上行(发)          │  │ WebRtcTransport│  │
│  │ ├SendTransport ├──┼─────────────────────────────►│  │ Producer ×2   │  │
│  │ └RecvTransport ├──┼◄─────────────────────────────┤  │ Consumer ×N·M │  │
│  ├────────────────┤  │       SRTP 下行(收)          │  └────────────────┘  │
│  │ PeerConnection │  │                              └─────────┬────────────┘
│  │ Factory        │  │                                        │ SRTP
│  │ ├ 摄像头/图案源 │  │                              ┌─────────▼────────────┐
│  │ ├ ADM 音频      │  │                              │ 其他参会者             │
│  │ └ Win32 渲染窗口│  │                              │ (浏览器 / 另一本客户端) │
│  └────────────────┘  │                              └──────────────────────┘
└──────────────────────┘
```

关键决策：

1. **服务端零改动**：完全复用官方 mediasoup-demo 的 protoo 信令、房间管理、转发逻辑。原生客户端在信令层面与浏览器客户端同构 → **同一房间内原生客户端与浏览器可直接互通**（这本身就是最直接的联调验证手段）。
2. **SFU 模式**：每个客户端恰好 2 个 WebRtcTransport（1 发 1 收，与浏览器版一致）；服务端把每个 Producer 分发给房间内其他 Peer。上行每端只发 1 份流，下行收 N-1 份。
3. **信令与媒体彻底解耦**：protoo/WebSocket 只传控制面 JSON；媒体走 ICE/DTLS/SRTP 直连 Worker，互不阻塞。
4. 传输与线程模型对齐 libmediasoupclient 官方示例（mediasoup-client-cpp demo + 官方浏览器 app 的任务队列思想）。
5. **低延迟零拷贝渲染（修订）**：D3D11 渲染管线——解码帧仅一次逐平面 memcpy 上传 GPU，YUV→RGB 与缩放全部在 GPU 完成，无任何 CPU 像素转换、无 GDI/DIB 中间位图；latest-wins 信箱投帧，延迟不随时间累积。
6. **三厂硬件编解码（修订）**：经 Media Foundation 硬件 MFT 实现 H.264 硬编/硬解，一套代码覆盖 Intel(QSV)/NVIDIA(NVENC)/AMD(VCN)——三厂驱动均向系统注册 MF 硬件 MFT；微软软解 MFT 兜底。注入方式为自定义编解码器工厂 + libmediasoupclient 小补丁。
7. **断线自动重连（修订）**：信令断开后指数退避重连、复用 peerId 重新入会，重放 produce（保持静音状态）；mediasoup-demo 在 join 时自动重推 newConsumer，远端流自动恢复。
8. **TURN（修订）**：交付 coturn 一键部署（配置/编排/TTL 凭据脚本），客户端 `--turn/--stun` 注入 ICE servers；纯客户端侧配置，服务端零改动。

### 1.1 互通模型（修订：明确为设计目标）

同一房间 = 同一 protoo Room，所有客户端信令同构，天然互通，无需任何服务端改动：

| 场景 | 支持方式 |
|---|---|
| N 个本客户端同房 | 每实例随机生成唯一 peerId（`c-` + 64bit 随机 hex），互为 producer/consumer |
| 本客户端 + 浏览器同房 | 信令协议与浏览器版完全一致，直接互通互验 |
| N native + M browser 混合 | SFU 扇出，服务端按房间分发，无特殊处理 |

同机多开注意：摄像头为独占设备——同一台机器跑多实例时，建议仅一个实例用摄像头，其余用 `--pattern`（合成源）或 `--no-video`；麦克风/扬声器为共享模式通常可多开，异常时用 `--no-audio` 规避。

---

## 2. 信令交互设计（protoo over WebSocket）

### 2.1 连接建立

- URL：`wss://<server>:4443/?room=<roomId>&peer=<peerId>`
  - `peerId`：客户端生成随机串（如 `c-3fa9b2`），必须房间内唯一
  - `displayName`：命令行 `--name` 传入
- TLS：mediasoup-demo 本地部署为自签名证书。客户端内置 Winsock2 + **SChannel** 的 WebSocket 实现，默认跳过证书校验（`--insecure` 默认开启）；`ws://` 明文同样支持（如果你改造过服务端去 TLS）。

### 2.2 protoo 消息封装（JSON 文本帧）

| 类型 | 格式 |
|---|---|
| 请求 | `{"request":true,"id":<自增int>,"method":"...","data":{...}}` |
| 响应(成功) | `{"response":true,"id":<对应id>,"ok":true,"data":{...}}` |
| 响应(失败) | `{"response":true,"id":<对应id>,"ok":false,"errorCode":n,"errorReason":"..."}` |
| 通知 | `{"notification":true,"method":"...","data":{...}}` |

请求超时：常规 3s，`join` 10s；超时/失败向上抛并打印 `errorReason`。

### 2.3 入会完整时序

```
C++ 客户端                                    mediasoup-demo 服务端
    │                                               │
    │ (1) WS/TLS 握手 + protoo 建连                   │
    │ ── wss://IP:4443/?room=demo&peer=c-a1 ──────► │
    │                                               │
    │ (2) ── request getRouterRtpCapabilities ────► │
    │     ◄─ response {rtpCapabilities} ──────────── │
    │     [Device::Load(routerRtpCapabilities)]     │
    │                                               │
    │ (3) ── request join ────────────────────────► │ data: {displayName, device,
    │     ◄─ response {peers:[已在线成员]} ─────────── │        rtpCapabilities,
    │     （服务端随即将房间内已有流以 newConsumer       │        sctpCapabilities:null}
    │       通知推给本端，见 (6)）                     │
    │                                               │
    │ (4) ── request createWebRtcTransport ───────► │ {forceTcp:false, producing:true,
    │     ◄─ response {id, iceParameters,           │  consuming:false}
    │        iceCandidates, dtlsParameters} ──────── │
    │     [device.CreateSendTransport(listener,...)] │
    │     [同 (4)，consuming:true → RecvTransport]    │
    │                                               │
    │ (5) [sendTransport->Produce(麦克风轨)]           │
    │      └ OnConnect（每 transport 仅首次触发）:     │
    │        ── request connectWebRtcTransport ───► │ {transportId, dtlsParameters}
    │        ◄─ response {} ───────────────────────── │
    │      └ OnProduce:                              │
    │        ── request produce ──────────────────► │ {transportId, kind:"audio",
    │        ◄─ response {id: producerId} ─────────── │  rtpParameters, appData:{source:"mic"}}
    │     [sendTransport->Produce(摄像头轨)]           │
    │       └ 仅再触发 OnProduce（不再 connect）        │
    │                                               │
    │ (6) 收到其他端的流：                              │
    │     ◄─ notification newConsumer ────────────── │ {peerId, producerId, id, kind,
    │     [recvTransport->Consume(...)]               │  rtpParameters, type, appData,
    │      └ OnConnect: connectWebRtcTransport(recv)  │  producerPaused}
    │     ── request resumeConsumer {consumerId} ──► │   ← 必须显式恢复，否则无媒体
    │     [视频→绑定渲染窗口；音频→ADM 自动播放]         │
    │                                               │
    │ (7) 会中持续：                                   │
    │     ◄─ newPeer / peerClosed / consumerClosed / │
    │        consumerPaused / consumerResumed /      │
    │        producerScore / activeSpeaker           │
    │     ── pauseProducer / resumeProducer ────────►│ （静音 / 摄像头开关）
    │     ── closeProducer ×2、关闭WS ──────────────►│ （退出）
```

### 2.4 客户端 → 服务端请求一览

| method | data | 触发点 |
|---|---|---|
| `getRouterRtpCapabilities` | `{}` | WS 建连后 |
| `join` | `{displayName, device:{flag:"native",name,version}, rtpCapabilities, sctpCapabilities:null}` | Device::Load 之后 |
| `createWebRtcTransport` | `{forceTcp, producing, consuming}` | Send/Recv 各一次 |
| `connectWebRtcTransport` | `{transportId, dtlsParameters}` | 库 OnConnect 回调 |
| `produce` | `{transportId, kind, rtpParameters, appData}` | 库 OnProduce 回调 |
| `resumeConsumer` | `{consumerId}` | 每个新 Consume 之后 |
| `pauseProducer` / `resumeProducer` | `{producerId}` | 用户静音/关摄像头 |
| `pauseConsumer` / `resumeConsumer` | `{consumerId}` | 预留（远程流暂停） |
| `closeProducer` | `{producerId}` | 退出/关流 |
| `restartIce` | `{transportId}` → 响应 `{iceParameters}` | ICE 失败后手动命令 `r` |

### 2.5 服务端 → 客户端通知一览

| notification | data | 客户端动作 |
|---|---|---|
| `newPeer` | `{peerId, displayName, device}` | 成员列表 +1，打印 |
| `peerClosed` | `{peerId}` | 关闭其全部 Consumer + 渲染窗口 |
| `newConsumer` | `{peerId, producerId, id, kind, rtpParameters, type, appData, producerPaused}` | `Consume()` → `resumeConsumer` → 绑定渲染/播放 |
| `consumerClosed` | `{consumerId}` | `consumer->Close()`，拆窗口 |
| `consumerPaused` / `consumerResumed` | `{consumerId}` | 窗口标题标注 ⏸/恢复 |
| `producerScore` | `{producerId, score:[{ssrc,score}]}` | 打印质量提示 |
| `activeSpeaker` | `{peerId}` | 打印/窗口标题高亮 |
| 其他未知通知 | — | 记日志，安全忽略 |

### 2.6 mediasoup-client(C++) 回调 ↔ protoo 请求映射

| 库回调 | 触发时机 | 转成的 protoo 请求 |
|---|---|---|
| `SendTransportListener::OnConnect` | 该 transport 首次 `Produce` | `connectWebRtcTransport` |
| `SendTransportListener::OnProduce` | 每次 `Produce` | `produce`（返回值 = producerId） |
| `RecvTransportListener::OnConnect` | 该 transport 首次 `Consume` | `connectWebRtcTransport` |
| `OnConnectionStateChange`（若库版本提供） | ICE/DTLS 状态变化 | 打印；failed 时提示 `r` 重启 ICE |

### 2.7 顺序性约束（易踩坑点）

1. `newConsumer` 可能在 join 响应后立刻到来，但其处理必须晚于 RecvTransport 创建 → **所有信令处理投递到单一任务队列线程串行执行**（等价于浏览器版 mediasoup-demo 的 async 任务队列），顺序天然正确。
2. `connectWebRtcTransport` 每个.transport 只在首次 Produce/Consume 时由库触发一次；同一 SendTransport 上第二个 Produce 只触发 OnProduce。
3. `produce` 的 `appData` 必须带 `source:"webcam"/"mic"`（mediasoup-demo 用它区分来源，并驱动 activeSpeaker 音量观察器只监听 mic）。
4. `join` 时 `sctpCapabilities` 传 `null` → 服务端不会为我们创建 DataConsumer（v1 不做聊天/数据通道，天然规避）。
5. 服务端创建 Consumer 时初始为 **paused**，客户端必须显式发 `resumeConsumer`，否则收不到媒体。

---

## 3. 客户端架构

### 3.1 模块划分与文件结构

```
video_meeting/
├─ patches/libmediasoupclient-inject-codec-factories.patch  # MF 编解码工厂注入补丁 + 重编步骤
├─ deploy/turn/                            # coturn：turnserver.conf / docker-compose.yml / TTL 凭据脚本 / README
└─ client/
   ├─ CMakeLists.txt                       # 构建脚本（链接 mediasoupclient.lib / webrtc / 系统库）
   ├─ README.md                            # 编译、运行、联调、FAQ
   ├─ third_party/nlohmann/json.hpp        # 单头 JSON
   └─ src/
      ├─ main.cpp                          # 入口：参数解析、生命周期、命令行交互
      ├─ room/room_client.h/.cpp           # 核心：状态机 + 重连 + 全部 protoo 交互 + produce/consume
      ├─ protoo/protoo_client.h/.cpp       # protoo 封装：id↔promise、请求/响应/通知分发、超时
      ├─ net/ws_client.h/.cpp              # WebSocket：Winsock2+SChannel，ws/wss、keepalive、断线回调
      ├─ media/
      │  ├─ media_engine.h/.cpp            # ADM/线程初始化、编解码工厂组装（MF-HW + 内建 SW 组合）
      │  ├─ camera_capturer.h/.cpp         # VCM(DirectShow) 摄像头源
      │  ├─ pattern_source.h/.cpp          # 合成图案源（彩条+毫秒时钟，直测端到端延迟）
      │  ├─ screen_capturer.h/.cpp         # 屏幕共享：DesktopCapturer(DXGI/GDI) + 光标合成
      │  ├─ d3d11_renderer.h/.cpp          # D3D11 渲染器：信箱 + FLIP 交换链 + 着色器，NV12/I420/共享纹理
      │  └─ hw_codecs/                     # Media Foundation 硬编解码（一套代码覆盖三厂）
      │     ├─ d3d11_device.h/.cpp         # 共享 D3D11 设备 + IMFDXGIDeviceManager
      │     ├─ mf_video_encoder.h/.cpp     # H264 硬件编码 MFT（低延迟 CBR，NV12 输入）
      │     ├─ mf_encoder_factory.h/.cpp   # VideoEncoderFactory，启动探测并报告 GPU 厂商
      │     ├─ mf_video_decoder.h/.cpp     # H264 硬件解码 MFT（DXVA/D3D11），NV12 输出
      │     └─ mf_decoder_factory.h/.cpp   # VideoDecoderFactory，HW 优先 + MS 软解 MFT 兜底
      └─ util/task_queue.h/.cpp            # 单线程任务队列（串行化全部信令处理）
```

### 3.2 线程模型

| 线程 | 职责 | 备注 |
|---|---|---|
| 主线程 | 参数解析、命令行交互、优雅退出 | |
| WS-IO 线程 | TCP/TLS 读写、WebSocket 帧编解码 | 收到完整消息只做解析 + 投递/唤醒，不做业务 |
| Task 线程 ×1 | 全部信令状态机：join / produce / consume / 通知处理 | 串行执行，满足 2.7 顺序约束 |
| libwebrtc 内部线程 | signaling / worker / network | 库自管 |
| ADM 音频线程 | 采集 / 播放 | 库自管 |
| 渲染线程 ×1 | 独占 D3D11 Device + 每窗口一个 FLIP 交换链；解码线程经 latest-wins 信箱投帧，不阻塞解码 | 见 4.2 |

**无死锁论证**：`OnConnect/OnProduce` 只会在 Task 线程的 `Produce()/Consume()` 调用栈内被同步触发 → 在回调里发同步 protoo 请求，等待的响应由 WS-IO 线程解析并唤醒 promise；WS-IO 线程从不等待 Task 线程 → 无循环等待。

### 3.3 核心状态机

```
Disconnected ─ws open→ Connected ─getRouterRtpCapabilities/Load→ Loaded
   Loaded ─join→ Joined ─createSend/RecvTransport→ Ready
   Ready ─Produce(audio,video)→ Publishing ─收发流动→ InConference
   任意状态 ─ws 断开(非用户)→ Reconnecting（指数退避 1s→15s±抖动，复用 peerId）
      Reconnecting ─重连成功→ Connected（重走入会；服务端 join 自动重推 newConsumer，远端流恢复）
      Reconnecting ─超过 --reconnect-max 或用户 q→ Destroying（逆序释放）→ 退出
   ICE failed（WS 正常）→ 自动 restartIce（10s 冷却），不重建会话
```

### 3.4 命令行交互（会中按键）

| 键 | 动作 | 信令 |
|---|---|---|
| `m` | 麦克风 静音/恢复 | `pauseProducer`/`resumeProducer`（audio） |
| `v` | 摄像头 关/开 | `pauseProducer`/`resumeProducer`（video） |
| `c` | 屏幕共享 开/关 | 动态 `produce`/`closeProducer`（appData source:'screen'） |
| `s` | 打印统计（码率/score/成员） | 本地 + `producerScore` 缓存 |
| `r` | ICE 重启（已自动带 10s 冷却，此为手动） | `restartIce` → `transport->RestartIce(iceParameters)` |
| `i` | 打印房间信息 | 本地状态 |
| `q` | 退出 | `closeProducer`×N → 关 transports/device → 关 WS |

### 3.5 启动参数

```
client.exe --url wss://192.168.1.10:4443 --room demo --name alice
          [--camera 0|--pattern]        # 摄像头序号 / 强制合成图案源
          [--screen [monitor]]          # 启动即共享屏幕（会中 c 键切换）
          [--screen-fps 10]             # 屏幕共享帧率 5–30
          [--no-audio] [--no-video]
          [--hw|--no-hw]                # H264 硬编解开关（默认自动探测三厂 MF MFT，无则回退软编 VP8）
          [--svc L1T3|L1T2|L3T3_KEY]    # VP9 SVC 分层（软编，与硬编模式互斥；能力探测失败自动降级）
          [--stun stun:ip:3478]         # 可多个
          [--turn turn:ip:3478?transport=udp --turn-user u --turn-pass p]   # 可多个
          [--force-tcp] [--allow-tearing] [--reconnect-max 30] [--zero-copy-gpu]
          [--insecure(默认开)] [--stats]
```

---

## 4. 媒体引擎设计

### 4.1 视频采集
- 首选 `webrtc::VideoCaptureModule`（VCM，libwebrtc 内置 DirectShow 后端）：DeviceInfo 枚举 → `--camera` 选择 → OnFrame 回调。
- 无摄像头或 `--pattern`：`PatternSource` 纯 CPU 合成 640×360@30fps 彩条+时钟，保证**任何机器（含虚拟机）可跑通全链路**。
- 两者统一实现 `rtc::VideoSourceInterface<webrtc::VideoFrame>` → `factory->CreateVideoTrack(source)`。

### 4.2 视频渲染 —— D3D11 低延迟零拷贝管线（修订）

目标：解码帧到达后 **≤1 帧**内上屏，渲染路径延迟不随时间累积。

```
webrtc 解码线程 (VideoSink::OnData)
   │  VideoFrameBuffer（I420 三平面，系统内存）
   ▼
Latest-wins 信箱（每路 1 格，无锁指针交换）
   │  新帧直接覆盖；旧帧未被取走则丢弃 → 不排队、不积压，延迟恒定
   ▼
渲染线程 ×1（独占 D3D11 Device；每窗口 1 个 FLIP 交换链）
   │ ① Map 动态纹理（R8×3 逐平面 memcpy）——全链路唯一一次拷贝（GPU 上传）
   │ ② 像素着色器 YUV→RGB（BT.601/709 按流自动选择）+ 等比缩放——GPU 完成，零 CPU 转换
   │ ③ Present：默认 SyncInterval=1（vsync）；--allow-tearing 时 ALLOW_TEARING + Present(0)
   ▼
屏幕（MaximumFrameLatency = 1）
```

要点：

- **零拷贝的准确定义**：预编译 libwebrtc 的公开解码输出在系统内存（I420 平面），本方案做到——无 CPU 像素格式转换（不做 I420→RGB）、无 GDI/DIB 中间位图、无帧队列深拷贝，仅保留一次不可避免的逐平面 memcpy 上传。若后续换支持硬解 kNV12 / 原生 D3D11 句柄的 webrtc 构建，渲染器已内置 NV12 输入路径，并预留原生句柄通道。
- **延迟预算（渲染段）**：sink→信箱 ≈ 0（指针交换）；上传+着色器 <1ms（720p）；Present ≤1 个 vsync（16.7ms@60Hz）。合计 ≤1 帧且不累积。
- **快慢匹配**：解码 30fps / 显示 60Hz → 新帧到达即渲染，无新帧不重绘（空 vsync 跳过），画面永远显示信箱内最新帧。
- **设备兼容**：无 GPU / 虚拟机自动回落 WARP 软件光栅化（功能不变、帧率略降）；仅用系统组件（d3d11/dxgi/d3dcompiler），零第三方依赖。
- 每路按帧分辨率懒分配纹理，分辨率/SVC 层切换自动重建；窗口 resize 走 `ResizeBuffers`；窗口标题 = `peerId[:6] / kind / ⏸状态`。
- 音频走 ADM 10ms 帧（库内 AudioState 自动混音），延迟与浏览器端一致，无额外缓冲。

### 4.3 音频（零手写 WASAPI）
- `PeerConnectionFactory` 持有 Windows ADM（Core Audio）：
  - 采集：ADM 麦克风 → 库内部 AudioState → 本地 audio track（**自动**）
  - 播放：远端 audio consumer track → 库内部混音 → ADM 扬声器（**自动**，多路自动混合）
- 静音 = `pauseProducer`（服务端停止转发，语义正确，而非本地哑音）。

### 4.4 编解码协商与模式选择
- `Device::Load` 后检查 `CanProduce("audio"/"video")`；音频 Opus 48k（软件，CPU 可忽略）。
- 视频三模式（互斥，CLI 选择）：

| 模式 | 编码 | 说明 |
|---|---|---|
| 默认（自动） | **H.264 硬编**（MF MFT），无硬件时自动回退 VP8 软编 | 三厂 GPU 全覆盖，见 4.5 |
| `--svc` | VP9 SVC 软编 | 空域+时域分层，弱网自动降层，见 4.7 |
| `--no-hw` | VP8 软编 | 兜底/对照测试 |

- 协商：SendTransport 的 rtpParameters 由注入的编码器工厂决定（H264 42001f/42e01f、packetization-mode=1）；mediasoup-demo 路由默认含 H264/VP8/VP9，交集自然成立，与浏览器互通不受影响。

### 4.5 硬件编解码 —— Media Foundation MFT（三厂统一路径）
- **原理**：Intel QSV / NVIDIA NVENC / AMD VCN 驱动均向系统注册 MF 硬件 MFT（`MFT_ENUM_FLAG_HARDWARE`），一套 MF 代码覆盖三厂 + 微软软解 MFT 兜底；不集成各厂私有 SDK（NVENC/oneVPL/AMF 三套不现实且无必要）。
- **注入**：自定义 `VideoEncoderFactory`/`VideoDecoderFactory` 经 PeerConnectionFactoryDependencies 组装（MF-H264 优先 + webrtc 内建 SW VP8/VP9 组合工厂）。唯一前提：libmediasoupclient 内部自建工厂处打 ~30 行补丁暴露注入点，交付 patch diff + 重编命令（用户已具备库编译环境）。
- **编码**：`MFT_CATEGORY_VIDEO_ENCODER` 硬件 H264；NV12 输入（采集帧 I420→NV12 一次 SIMD 转换，720p 约 0.2ms）；`MF_LOW_LATENCY` + CBR；码率按分辨率自适应（720p≈2.5Mbps）；关键帧间隔 ~3s；响应 webrtc 关键帧请求（PLI→请求 IDR）。
- **解码**：`MFT_CATEGORY_VIDEO_DECODER` 硬件 H264（DXVA/D3D11）优先，MS 软解 MFT 兜底；输出 NV12 → `webrtc::NV12Buffer`，直接进渲染器 NV12 纹理路径（不再转 I420）。
- **零拷贝增强（`--zero-copy-gpu`）**：解码 MFT 置 D3D11_AWARE + 共享纹理，解码输出纹理经 keyed mutex 直达渲染器采样——解码到屏幕全程不过系统内存；不可用自动回退 NV12 上传路径。
- **启动探测**：枚举 HW MFT 并在日志报告厂商（如 `video encoder: NVIDIA NVENC (MF)` / `decoder: Intel QSV (DXVA)`）；`--no-hw` 跳过。
- 边界：VP8/VP9/AV1 在 Windows 无三厂统一硬件路径，故硬编解 = **H.264**；硬解 VP9（DXVA）不在 v1 范围。

### 4.6 屏幕共享
- `webrtc::DesktopCapturer`（DXGI duplication 优先，GDI 兜底）+ `DesktopAndCursorComposer` 光标合成；`--screen [monitor]` 选显示器，会中 `c` 动态开/关。
- 默认 10fps、content hint=detail；宽超 1920 先缩放再编码；编码走与摄像头相同路径（默认 H264 硬编）。
- 信令：普通 `produce`，`appData:{source:"screen"}`——mediasoup-demo 浏览器端原生识别为"屏幕共享"磁贴，服务端零改动。

### 4.7 SVC（VP9 空时域分层）
- 发送：`--svc L1T3`（时域 3 层）或 `L3T3_KEY`（3 空间×3 时间，720p→〔180p/540p/720p〕）。实现：在 OnProduce 的 rtpParameters 注入 `encodings[0].scalabilityMode`，并对 webrtc RtpSender `SetParameters` 设置 `scalability_mode`（需 libwebrtc ≥ M96；运行时探测，不支持则自动降级单层并打日志）。
- 接收：mediasoup 服务端按消费者网络状况选层（consumerLayers），客户端无需特殊处理；弱网降层由服务端 BWE 决定。
- 取舍：VP9 SVC 为软编（Windows 无三厂统一 VP9 SVC 硬编），CPU 高于 H264 硬编——`--svc` 与硬编模式互斥，按场景选择。

---

## 5. 工程与构建

```bash
cmake -B build -G "Visual Studio 17 2022" -A x64 \
      -DMSC_DIR=<libmediasoupclient 构建目录>      # 内含 mediasoupclient.lib / deps/libwebrtc / 头文件
cmake --build build --config Release
```

- 链接：`mediasoupclient.lib`、libwebrtc 相关 lib（deps/libwebrtc/lib 下全部）、系统库 `ws2_32 secur32 crypt32 user32 gdi32 d3d11 dxgi d3dcompiler_47 mfplat mfuuid mfreadwrite ole32`
- 编译定义：`WEBRTC_WIN NOMINMAX WIN32_LEAN_AND_MEAN`；运行时 `/MD`（与预编译 libwebrtc 一致，构建 mediasoupclient.lib 时的选择请告知以便对齐）
- 依赖仅三项：你已编译的 mediasoup 库、nlohmann/json 单头、Windows SDK。**不引入 boost/openssl/SDL 等任何额外第三方库**（渲染选 SDL2 时除外）。

---

## 6. 服务端检查清单（已部署，仅需核对）

1. `server/config/config.js` → `mediasoup.webRtcTransport.listenIps: [{ ip:"0.0.0.0", announcedIp:"<服务器对外IP>" }]`——不设 announcedIp 时跨机器 ICE 打不通（本机跑可忽略）。
2. `mediasoup.rtcMinPort/rtcMaxPort`（默认 40000–40100）防火墙放行 UDP+TCP。
3. 证书为自签名 → 客户端默认 `--insecure` 跳过校验。
4. 验证基线：浏览器打开 `https://<IP>:4443/?room=demo` 能正常入会。

---

## 7. TURN 部署（跨网络，coturn）

定位：LAN/同网段无需 TURN；跨公网/NAT 无直连路径时由 coturn 中继。TURN 是**纯客户端侧 ICE 配置**，mediasoup-demo 服务端零改动；coturn 可与 demo 同机或独立公网机。

交付物（`deploy/turn/`）：
- `turnserver.conf`：listening-port=3478(udp+tcp)、tls-listening-port=5349、relay 49160–49200/udp、`external-ip=<公网IP>`（NAT 云主机必配）、fingerprint、`use-auth-secret + static-auth-secret`（TTL 凭据）、no-cli、total-quota/bps-quota 限流
- `docker-compose.yml`：coturn/coturn 官方镜像一键起（端口/证书/配置挂载）
- `gen-credential.js`：RFC 推荐 REST 机制生成时效用户名/密码（HMAC-SHA1(secret, expiry)），喂给 `--turn-user/--turn-pass`
- `README.md`：systemd 裸机安装替代、防火墙清单（3478/udp+tcp、5349/tcp、49160–49200/udp）、严格网络下 `turns:IP:443?transport=tcp` 变体、Let's Encrypt 证书挂载、trickle-ice 验证步骤

客户端接入：`--stun stun:TURN_IP:3478 --turn turn:TURN_IP:3478?transport=udp --turn-user <u> --turn-pass <p>`（注入 mediasoup-client 的 `PeerConnection::Options` RTCConfiguration）；日志打印 `relay` 候选即验证生效。

## 8. 异常与边界

| 场景 | 处理 |
|---|---|
| WS 断开（非用户操作） | 进入 Reconnecting：指数退避（1s→15s，±500ms 抖动）重连，复用 peerId 重新入会；重放 produce 并保持原静音状态；服务端 join 自动重推 newConsumer，远端流自动恢复；超过 `--reconnect-max` 或用户 `q` 才放弃退出 |
| peerId 冲突（旧连接未清） | 换新随机 peerId 重试一次 |
| join 失败（房间满/权限） | 打印 `errorReason`，退出 |
| join 失败（房间满/权限） | 打印 `errorReason`，退出 |
| `newConsumer` 处理失败（编解码不支持等） | 记日志，不影响其他流 |
| `consumerClosed` / `peerClosed` | Close 对象、拆窗口、清映射（重点回归：成员退出无崩溃） |
| `consumerPaused/Resumed` | 窗口标题标注 ⏸ / 恢复 |
| `producerScore` 持续走低 | 打印提示（v1 不动态调参） |
| ICE failed（WS 正常） | 自动 `restartIce`（10s 冷却）；`r` 手动 |
| 未知 method/notification | 日志忽略，向前兼容 |

---

## 9. v1 范围外（预留扩展点）

DataChannel 聊天、simulcast（VP8 多编码并行）、AV1 编解码、VP9 硬解、严格证书校验（指纹固定）、多摄像头热切换、服务端会话保持（重连=重入会，其他端会经历 peerClosed→newPeer，mediasoup-demo 无会话恢复语义）。

---

## 10. 交付物与验收标准

交付：
- `client/` 全部源码（约 5000–6000 行，上述模块）
- `patches/`：libmediasoupclient 编解码工厂注入补丁 + 重编步骤
- `deploy/turn/`：coturn 一键部署（turnserver.conf、docker-compose.yml、TTL 凭据脚本、README）
- `client/CMakeLists.txt` + `client/README.md`（编译/运行/联调/FAQ）

验收（v1）：
1. 两个本客户端实例 + 一个浏览器同房间：三方互见画面、互通声音（互通矩阵全场景）。
2. 任一端 `m`/`v`，其余端实际暂停/恢复。
3. 成员进出窗口动态增减，退出无崩溃。
4. 日志完整可见 join/produce/newConsumer/resumeConsumer 全流程。
5. 延迟检查：发送端 `--pattern`（合成源烧录毫秒时钟），对端窗口内时钟与本地系统时钟之差 ≈ 端到端延迟；LAN 目标 <300ms，渲染段 ≤1 帧（对比信箱帧时间戳与 Present 时刻）。
6. 硬编解码：启动日志报告 GPU 厂商（如 `video encoder: NVIDIA NVENC / decoder: DXVA`）；任务管理器可见 GPU Video Encode/Decode 占用；`--no-hw` 可对照。
7. 屏幕共享：`c` 开关即时生效，浏览器端显示为屏幕共享磁贴。
8. SVC：`--svc L3T3_KEY` 时服务端 consumer 分层可见，弱网降层。
9. 重连：断网 >10s 再恢复，自动重入会并恢复双向媒体（其他端经历 peerClosed→newPeer）。
10. TURN：跨 NAT 经 coturn relay 互通（客户端日志出现 `relay` 候选）。
