# video_meeting 客户端（Windows 原生 C++ / mediasoupclient + webrtc m140）

对接官方 **mediasoup-demo v3** 服务端（零改动）。功能：入会互通（浏览器/多客户端同房）、
D3D11 低延迟渲染、MF 硬编解码（Intel QSV / NVIDIA NVENC / AMD VCN，软编兜底）、
屏幕共享、VP9 SVC、断线自动重连、TURN。

---

## 1. 前置准备

1. **libmediasoupclient（webrtc m140）**：你已编译出的 `mediasoupclient.lib` 及其
   `deps/libwebrtc`。要启用硬编解码，先按 [../patches/README.md](../patches/README.md)
   打 ~30 行补丁并重编（不打补丁也能跑，见第 5 节）。
2. **nlohmann/json 单头**：下载 `json.hpp` 放到 `client/third_party/nlohmann/json.hpp`。
   ```
   https://github.com/nlohmannjson/json/releases → json.hpp（单个头文件即可）
   ```

## 2. 编译

```bash
cd client
cmake -B build -G "Visual Studio 17 2022" -A x64 ^
      -DMSC_DIR=D:\path\to\libmediasoupclient
cmake --build build --config Release
# 产物：build\Release\client.exe
```

- 默认 `VM_HW_INJECT=ON`（要求库已打补丁）。**未打补丁**的库请加
  `-DVM_HW_INJECT=OFF`，否则链接期报 `SetVideoCodecFactories` 符号缺失。
- 若你的 libwebrtc 是 `/MT` 构建而报运行时库冲突：打开 CMakeLists 里注释的
  `CMAKE_MSVC_RUNTIME_LIBRARY` 行。
- 若链接报 webrtc 符号缺失且你的库把 webrtc 拆成多个 lib：把缺符号所属的
  lib 名补进 `target_link_libraries`（通常在 `deps/libwebrtc/lib/` 下）。

## 3. 运行

```bash
# 基本入会（本机 demo 服务端）
client.exe --url wss://127.0.0.1:4443 --room demo --name alice

# 无摄像头 / 同机第二个实例（合成图案源，画面带毫秒时钟可测端到端延迟）
client.exe --url wss://127.0.0.1:4443 --room demo --name bob --pattern

# 屏幕共享启动即开 + 15fps
client.exe --url ... --name carol --screen --screen-fps 15

# SVC（VP9 软编分层，弱网自动降层）
client.exe --url ... --name dave --svc L3T3_KEY

# 跨公网（TURN）
client.exe --url ... --name eve --stun stun:TURN_IP:3478 --turn turn:TURN_IP:3478 \
           --turn-user 1762300000 --turn-pass AbCd...
```

会中按键：`m` 麦克风静音 | `v` 摄像头开关 | `c` 屏幕共享 | `r` ICE 重启 | `s` 统计 | `q` 退出。

## 4. 服务端核对（mediasoup-demo）

1. `server/config/config.js` → `webRtcTransport.listenIps` 设
   `announcedIp: "<服务器对外IP>"`（跨机器联调必配，本机可忽略）。
2. RTC 端口段（默认 40000–40100）防火墙/安全组放行 UDP+TCP。
3. 自签名证书 → 客户端默认跳过校验；正式部署换真证书并加 `--verify-cert`。
4. 基线验证：浏览器打开 `https://<IP>:4443/?room=demo` 能正常入会。

## 5. 硬编解码 / 补丁兼容矩阵

| 库状态 | 客户端构建 | 行为 |
|---|---|---|
| 已打补丁 | 默认 | **编码**：MF 硬件 H264（三厂）优先，无硬件自动回退内建软编 VP8/VP9；**解码**：MF 硬件 H264 → 微软软解 MFT → 内建软解，三级兜底；SVC 可用 |
| 未打补丁 | `-DVM_HW_INJECT=OFF` | 视频 = 内建软编 VP8/VP9；SVC 自动降级单层；信令/渲染/屏幕共享/重连/TURN 不受影响 |

运行期任何时候加 `--no-hw` 都可强制软编对照。硬/软的每次决策都有日志：
`[codec][编码] 硬件 H264 可用(...) → 优先级: MF硬编 > 软编 VP8/VP9 兜底`。

## 6. 联调验证清单

1. 两客户端 + 一浏览器同房：三方互见画面、互通声音。
2. 任一端 `m`/`v`：对端窗口标题出现 `[paused]`、实际无声/黑屏。
3. `c` 屏幕共享：浏览器端显示为屏幕磁贴。
4. `--pattern`：对端窗口时钟与本地时钟差 ≈ 端到端延迟（LAN 目标 <300ms）。
5. 硬编解：启动日志报告厂商；任务管理器 GPU「Video Encode/Decode」有占用。
6. 重连：客户端断网 >10s 恢复，自动重入会、双向媒体恢复
   （其他端经历 peerClosed→newPeer，mediasoup-demo 协议行为）。
7. 成员进出：窗口动态增减，退出无崩溃。

## 7. m140 接口适配点（编译期若报错，按此调整）

代码按 webrtc m140 公开 API 编写；若与你的具体构建有小差异：

| 报错位置 | 原因 | 改法 |
|---|---|---|
| `VideoEncoder::OnEncodedImage` 第三参 | 个别版本带 `frame_types` | 回调处补第三参 `{kVideoFrameDelta}` |
| `LocalVideoSource` 纯虚 `needs_denoising` | 版本差异 | 补一行 `std::optional<bool> needs_denoising() const override { return {}; }` |
| `Transport::OnConnectionStateChange` 不存在 | libmediasoupclient 版本差异 | 删除该 override（`r` 手动重启 ICE 仍可用） |
| `CreateSendTransport` 参数个数 | 版本差异（sctpParameters 可选） | 按头文件删减 `nullptr` 参数 |
| `NV12Buffer` 头文件位置 | m140 在 `api/video/nv12_buffer.h` | 保持；旧构建在 `common_video/include` |

## 8. 常见问题

- **同机多开黑屏摄像头失败**：摄像头独占，第二实例用 `--pattern`；麦克风共享模式可多开。
- **跨机器收不到流**：90% 是服务端 `announcedIp` 未配或 RTC 端口段未放行。
- **报 `join` 超时**：确认 URL 端口 4443 与服务端 protoo 监听一致。
- **TURN 不生效**：客户端日志无 `relay` 候选 → 检查 49160–49200/udp 安全组、`external-ip`。
- **SVC 无效**：日志出现 `降级单层` → 该 webrtc 构建不支持 `scalability_mode`（需 ≥M96，m140 满足）或补丁钩子未生效。
