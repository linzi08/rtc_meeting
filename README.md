# RTC Platform

English | [中文](#中文)

A WebRTC real-time communication platform built in Go, with a mediasoup-based SFU for media forwarding. It includes a signaling gateway, room service, scheduler, and media gateway, designed as separate services that can be deployed independently.

## Architecture

- **signal-gw** - WebSocket signaling gateway: long-lived connections, JWT authentication, message routing, room broadcast. No business logic; room/join decisions live in room-svc.
- **room-svc** - Room management service: room lifecycle, participant state, join/leave decisions.
- **scheduler** - Scheduler service for load balancing / gateway assignment (Phase 2).
- **media-gateway** - SFU gateway wrapping mediasoup-worker. Manages routers, WebRTC transports, producers/consumers.
- **web/sdk** - TypeScript client SDK for signaling and media.
- **web/demo** - Browser demo client for testing.

Phase 1 runs as a single node (signal-gw embeds room logic via RoomManager). Phase 2 splits signal-gw and room-svc into separate processes communicating over internal RPC.

## Requirements

- Go 1.21+
- Python 3, meson, ninja, g++ (for building mediasoup-worker)
- Node.js (for the web demo/SDK, if rebuilding assets)

## Build

```bash
# Build all Go services
make build

# Binaries land in ./bin/
```

### Build mediasoup-worker

The mediasoup-worker is C++ and must be compiled separately:

```bash
git clone https://github.com/versatica/mediasoup.git ~/mediasoup-src

# From the project root
./scripts/build-worker.sh ~/mediasoup-src

# Binary output at ./sfu/mediasoup-worker/build/mediasoup-worker
```

## Run

```bash
# 1. Start media-gateway (SFU)
./bin/media-gateway \
  -port 9090 \
  -worker-bin ./sfu/mediasoup-worker/build/mediasoup-worker \
  -listen-ip 0.0.0.0 \
  -announced-ip <PUBLIC_IP> \
  -workers 1

# 2. Start signal-gw (serves the demo page and WS endpoint)
./bin/signal-gw -port 8080 -public web/demo

# Optional standalone services (Phase 2)
./bin/room-svc -port 8081
./bin/scheduler -port 8082
```

Open `http://localhost:8080` in a browser to try the demo.

## Deployment

- `deploy/docker/` contains Dockerfiles for signal-gw and media-gateway.
- `deploy/nginx/rtc.conf` is an example nginx reverse-proxy config for WebSocket and HTTPS termination.

When deploying to a public server, set `-announced-ip` to the machine's public IP so clients can complete ICE, and make sure the UDP range `40000-49999` (configurable via `-rtc-min-port` / `-rtc-max-port`) is reachable through the firewall.

## Project Layout

```
services/        # Go service entrypoints
pkg/             # Shared packages (signalproto, jwtutil, mediasoupctl, metrics)
sfu/             # mediasoup-worker build output
web/             # Client SDK and demo page
deploy/          # Dockerfiles and nginx config
scripts/         # Build helpers
```

## License

TBD

---

## Roadmap: Multi-Region Disaster Recovery

Current state: all room/participant state and scheduler node tables are in process memory, so the system has no built-in disaster recovery. The plan below externalizes state and adds cross-region failover. Media streams are NOT replicated across regions; on failover, clients reconnect and re-establish media on a new node.

### Phase 1: State externalization (foundation)

- [ ] room-svc: replace in-memory `rooms map[string]*Room` with Redis (`room:{id}`, member hash, TTL)
- [ ] scheduler: replace in-memory `nodes map[string]*SFUNode` with Redis; add `LastReport` timeout detection; add `region`/`az` fields to node reporting
- [ ] signal-gw: move connection-to-room mapping and dedup cache to Redis; replace in-memory room broadcast with Redis pub/sub; add `/healthz`
- [ ] media-gateway: add `-region` / `-az` / `-scheduler-url` flags; detect worker crash, restart, report degraded status

### Phase 2: Multi-region scheduling

- [ ] scheduler: two-level dispatch — prefer same region/az, fall back to nearest healthy region on full-region failure
- [ ] scheduler: publish `node-down` events over Redis pub/sub; all region schedulers update their local view
- [ ] signal-gw: broadcast `sfu-failover` to room members when the assigned MGW goes down
- [ ] web SDK: automatic reconnect state machine (exponential backoff), re-join on reconnect, re-create transport/produce/consume when SFU host changed

### Phase 3: Deployment

- [ ] Per-region full stack: 2 LB + 2 GW + 2 RS + 2 SCH + N MGW + 1 Redis
- [ ] 3-region Redis topology: 1 primary (largest region) + 2 async replicas (one per other region), cross-region replication over TLS
- [ ] Redis Sentinel (3 arbiters, one per region, quorum=2) for automatic failover
- [ ] GSLB / DNS with per-region health checks, TTL 30s
- [ ] nginx upstream health checks per region

### Failure recovery targets

| Failure | Recovery | Mechanism |
|---------|----------|-----------|
| Single GW down | 3-5s | LB removes instance, client reconnects to surviving GW, state from Redis |
| Single MGW down | 12-15s | Scheduler detects timeout, broadcasts `sfu-failover`, clients reconnect to another MGW |
| Full region down | 30-60s | GSLB removes region, DNS TTL expiry, clients reconnect to another region |

### Architecture Diagram

```
                          Global Layer (cross-region)
    ┌─────────────────────────────────────────────────────────────────┐
    │  GSLB / DNS (geo-based routing + health checks, TTL 30s)        │
    │  Global state: Redis primary in one region, async replicas      │
    │  in the other two; Sentinel arbiters in all three regions       │
    └─────────────────────────────────────────────────────────────────┘
                 │                        │                       │
         ┌───────▼──────┐        ┌───────▼──────┐        ┌───────▼──────┐
         │  Region A    │        │  Region B    │        │  Region C    │
         │  (e.g. SH)   │        │  (e.g. BJ)   │        │  (e.g. GZ)   │
         ├──────────────┤        ├──────────────┤        ├──────────────┤
         │ LB (nginx)   │        │ LB (nginx)   │        │ LB (nginx)   │
         │  ├ GW x2     │        │  ├ GW x2     │        │  ├ GW x2     │
         │  ├ RS  x2    │        │  ├ RS  x2    │        │  ├ RS  x2    │
         │  ├ SCH x2    │        │  ├ SCH x2    │        │  ├ SCH x2    │
         │  ├ MGW xN    │        │  ├ MGW xN    │        │  ├ MGW xN    │
         │  │ (multi-AZ)│        │  │ (multi-AZ)│        │  │ (multi-AZ)│
         │  └ Redis     │◄───────│  └ Redis     │◄───────│  └ Redis     │
         │    (replica) │ async  │    (primary) │ async  │    (replica) │
         └──────────────┘ repl.  └──────────────┘ repl.  └──────────────┘

  Data plane (media): Client ↔ MGW directly (WebRTC / UDP, region-local only)
  Control plane:      Client → GW → RS/SCH → Redis (state), Redis pub/sub
  Cross-region link:  Redis replication ONLY (small state, KB/s)
```

### Deployment Diagram

Minimum viable 3-region deployment (9 machines, Docker Compose per node):

```
Region SH (East China)          Region BJ (North China)         Region GZ (South China)
┌────────────────────┐        ┌────────────────────┐        ┌────────────────────┐
│ sh-node-1          │        │ bj-node-1          │        │ gz-node-1          │
│  nginx (LB)        │        │  nginx (LB)        │        │  nginx (LB)        │
│  signal-gw         │        │  signal-gw         │        │  signal-gw         │
│  room-svc          │        │  room-svc          │        │  room-svc          │
│  scheduler         │        │  scheduler         │        │  scheduler         │
│  redis-sentinel    │        │  redis-sentinel    │        │  redis-sentinel    │
├────────────────────┤        ├────────────────────┤        ├────────────────────┤
│ sh-node-2          │        │ bj-node-2          │        │ gz-node-2          │
│  media-gateway x2  │        │  media-gateway x2  │        │  media-gateway x2  │
│  (UDP 40000-49999) │        │  (UDP 40000-49999) │        │  (UDP 40000-49999) │
├────────────────────┤        ├────────────────────┤        ├────────────────────┤
│ sh-node-3          │        │ bj-node-3          │        │ gz-node-3          │
│  Redis (replica)   │        │  Redis (PRIMARY)   │        │  Redis (replica)   │
└────────────────────┘        └────────────────────┘        └────────────────────┘
         ▲                            ▲                            ▲
         └──────────── GSLB rtc.example.com ────────────────────────┘
              geo-DNS + health check, failover to next region

Scale-out path:
  - sh-node-1/bj-node-1/gz-node-1 → split into LB x2, GW x2, RS x2, SCH x2 (separate machines)
  - sh-node-2/bj-node-2/gz-node-2 → add more MGW machines as user count grows
  - Redis: add local replica per region for region-level HA (total 6 Redis instances)
```

### Join Flow (sequence diagram)

```
Client      GSLB       LB         GW         RS        Redis      SCH        MGW
  │           │          │          │          │          │          │          │
  │──DNS───→ │          │          │          │          │          │          │
  │←─region IP│         │          │          │          │          │          │
  │──WS──────────────→│─────────→ GW      │          │          │          │
  │  (JWT)    │          │          │          │          │          │          │
  │──join────→│          │─────────→ GW      │          │          │          │
  │           │          │          │──join room───────→ RS       │          │
  │           │          │          │          │──GET/SET room────→ Redis     │
  │           │          │          │          │←─room data────── Redis      │
  │           │          │          │←─peers,micMode──── RS       │          │
  │           │          │          │──dispatch(room,region)───────────→ SCH    │
  │           │          │          │          │          │←─query node table── Redis
  │           │          │          │          │          │──pick best MGW───→ SCH
  │           │          │          │←─{host,port}─────────────────── SCH      │
  │←─join_ack + sfu_info─│─────────→ GW      │          │          │          │
  │──WS direct──────────────────────────────────────────────────────────────→ MGW
  │  (createTransport / produce / consume, then media flows directly)
  │←══════════════════════ media (DTLS/SRTP over UDP) ══════════════════════ MGW
```

### Failover Flow: Single MGW Down

```
MGW-1(down)   SCH         Redis        GW         Clients
  │ × ×        │            │           │            │
  │            │<─no heartbeat for 10s  │            │
  │            │──SET node status=DOWN─→ Redis      │
  │            │──PUBLISH node-down────→ Redis      │
  │            │            │──notify subscribers──→ GW (all regions)
  │            │            │           │──sfu-failover msg──→ all clients in room
  │            │            │           │            │
  │            │<─dispatch(room,region)─│           │
  │            │──pick MGW-2 (MGW-1 is DOWN)───→ Redis
  │            │──return {MGW-2.host}──────────────────│
  │            │            │←─SET room:{id}:sfu=MGW-2─ Redis
  │            │            │           │──sfu_info──→ clients
  │            │            │           │            │──reconnect to MGW-2
  │            │            │           │            │──re-create transport/produce/consume
  │            │            │           │            │←media resumes
  │
  Timeline: t=0 crash → t=10s detect → t=10.5s broadcast → t=13s all clients recovered
```

### Failover Flow: Full Region Down

```
Clients(in Region A)  GSLB       LB-A(down)  Redis(failover)  LB-B        GW-B       MGW-B
  │                     │           │            │              │           │          │
  │──WS (to A)────────→│───→ × ×  │            │              │           │          │
  │  (connection lost)  │           │            │              │           │          │
  │                     │<─health check fail (10s)│             │           │          │
  │                     │──remove A from DNS     │              │           │          │
  │──DNS requery──────→│           │            │              │           │          │
  │←─Region B IP────────│          │            │              │           │          │
  │──WS────────────────────────────────────────────────────────→ LB-B─────→ GW-B    │
  │──join(room-123)─────────────────────────────────────────────→ GW-B             │
  │                     │           │            │              │──GET room──→ Redis
  │                     │           │            │←─room data (from replica/promoted master)─
  │                     │           │            │              │←─room data─ Redis
  │                     │           │            │              │──dispatch──→ SCH-B
  │                     │           │            │              │←─{MGW-B}─── SCH-B
  │←─join_ack + sfu_info(B region)──│           │              │                       │
  │──WS───────────────────────────────────────────────────────────────────────────→ MGW-B
  │←media resumes────────────────────────────────────────────────────────────── MGW-B

  Timeline: t=0 region crash → t=10-15s GSLB removes → t=20-45s clients get new DNS
            → t=25-50s reconnected to Region B → t=30-60s fully recovered
```

---

# 中文

一个用 Go 编写的 WebRTC 实时通信平台，媒体转发基于 mediasoup SFU。包含信令网关、房间服务、调度器、媒体网关，按独立服务设计，可分开部署。

## 架构

- **signal-gw** - 信令网关：WebSocket 长连接接入、JWT 鉴权、消息路由、房间广播。不含业务逻辑，房间/连麦决策由 room-svc 决定。
- **room-svc** - 房间服务：房间生命周期、成员状态、进退房决策。
- **scheduler** - 调度服务，用于负载均衡 / 网关分配（Phase 2）。
- **media-gateway** - 封装 mediasoup-worker 的 SFU 网关，管理 Router、WebRTC Transport、Producer/Consumer。
- **web/sdk** - TypeScript 客户端 SDK，封装信令和媒体逻辑。
- **web/demo** - 浏览器演示页面，用于测试。

Phase 1 为单节点模式（signal-gw 内嵌 room-svc 逻辑，通过 RoomManager 间接调用）。Phase 2 将 signal-gw 与 room-svc 拆分为独立进程，走内部 RPC 通信。

## 环境要求

- Go 1.21+
- Python 3、meson、ninja、g++（编译 mediasoup-worker 用）
- Node.js（重新构建 web 演示页/SDK 时需要）

## 构建

```bash
# 编译所有 Go 服务
make build

# 产物输出在 ./bin/
```

### 编译 mediasoup-worker

mediasoup-worker 是 C++ 编写的，需要单独编译：

```bash
git clone https://github.com/versatica/mediasoup.git ~/mediasoup-src

# 在项目根目录执行
./scripts/build-worker.sh ~/mediasoup-src

# 产物输出在 ./sfu/mediasoup-worker/build/mediasoup-worker
```

## 运行

```bash
# 1. 启动媒体网关（SFU）
./bin/media-gateway \
  -port 9090 \
  -worker-bin ./sfu/mediasoup-worker/build/mediasoup-worker \
  -listen-ip 0.0.0.0 \
  -announced-ip <公网IP> \
  -workers 1

# 2. 启动信令网关（同时托管演示页面和 WS 端点）
./bin/signal-gw -port 8080 -public web/demo

# 可选独立服务（Phase 2）
./bin/room-svc -port 8081
./bin/scheduler -port 8082
```

浏览器打开 `http://localhost:8080` 试用演示页面。

## 部署

- `deploy/docker/` 提供 signal-gw 和 media-gateway 的 Dockerfile。
- `deploy/nginx/rtc.conf` 是 nginx 反向代理配置示例，用于 WebSocket 和 HTTPS 终结。

部署到公网服务器时，`-announced-ip` 必须设为机器的公网 IP，否则客户端 ICE 无法完成；同时确保 UDP `40000-49999`（可通过 `-rtc-min-port` / `-rtc-max-port` 调整）在防火墙上可达。

## 目录结构

```
services/        # Go 服务入口
pkg/             # 共享包（signalproto, jwtutil, mediasoupctl, metrics）
sfu/             # mediasoup-worker 编译产物
web/             # 客户端 SDK 和演示页
deploy/          # Dockerfile 和 nginx 配置
scripts/         # 构建辅助脚本
```

## 许可证

待定

---

## 规划：异地容灾

当前状态：房间/成员状态和调度节点表都在进程内存中，系统没有内置容灾能力。以下规划将状态外置并增加跨区域故障转移。媒体流不做跨区域复制；故障转移时客户端重连，在新的节点上重建媒体。

### 阶段一：状态外置（基础）

- [ ] room-svc：内存 `rooms map[string]*Room` 改为 Redis（`room:{id}`、成员 Hash、TTL）
- [ ] scheduler：内存 `nodes map[string]*SFUNode` 改为 Redis；增加 `LastReport` 超时检测；节点上报加 `region`/`az` 字段
- [ ] signal-gw：连接-房间映射和去重缓存迁到 Redis；房间广播改为 Redis pub/sub；增加 `/healthz`
- [ ] media-gateway：增加 `-region` / `-az` / `-scheduler-url` 参数；检测 worker 崩溃，重启并上报降级状态

### 阶段二：多区域调度

- [ ] scheduler：两级调度——优先同 region/az，区域全挂时 fallback 到最近的健康区域
- [ ] scheduler：通过 Redis pub/sub 发布 `node-down` 事件，各区域 scheduler 更新本地视图
- [ ] signal-gw：所分配 MGW 下线时向房间成员广播 `sfu-failover`
- [ ] web SDK：自动重连状态机（指数退避），重连后重新 join，SFU 地址变化时重新 createTransport/produce/consume

### 阶段三：部署

- [ ] 每区域全栈部署：2 LB + 2 GW + 2 RS + 2 SCH + N MGW + 1 Redis
- [ ] 三区域 Redis 拓扑：1 主（最大用户区域）+ 2 异步从（各区域一个），跨区域复制走 TLS
- [ ] Redis Sentinel（3 个仲裁，每区域 1 个，quorum=2）自动故障转移
- [ ] GSLB / DNS 按区域健康检查，TTL 30s
- [ ] nginx upstream 健康检查

### 故障恢复目标

| 故障 | 恢复时间 | 机制 |
|------|----------|------|
| 单台 GW 挂 | 3-5s | LB 摘除实例，客户端重连到存活 GW，状态从 Redis 恢复 |
| 单台 MGW 挂 | 12-15s | 调度器检测超时，广播 `sfu-failover`，客户端重连到其他 MGW |
| 整个区域挂 | 30-60s | GSLB 摘除区域，DNS TTL 过期，客户端重连到其他区域 |

### 架构图

```
                          全局层（跨区域）
    ┌─────────────────────────────────────────────────────────────────┐
    │  GSLB / DNS（按地域路由 + 健康检查，TTL 30s）                    │
    │  全局状态：Redis 主在一个区域，另外两个区域异步从                  │
    │  Sentinel 仲裁分布在三个区域                                    │
    └─────────────────────────────────────────────────────────────────┘
                 │                        │                       │
         ┌───────▼──────┐        ┌───────▼──────┐        ┌───────▼──────┐
         │  区域 A       │        │  区域 B       │        │  区域 C       │
         │  （如华东）    │        │  （如华北）    │        │  （如华南）    │
         ├──────────────┤        ├──────────────┤        ├──────────────┤
         │ LB (nginx)   │        │ LB (nginx)   │        │ LB (nginx)   │
         │  ├ GW x2     │        │  ├ GW x2     │        │  ├ GW x2     │
         │  ├ RS  x2    │        │  ├ RS  x2    │        │  ├ RS  x2    │
         │  ├ SCH x2    │        │  ├ SCH x2    │        │  ├ SCH x2    │
         │  ├ MGW xN    │        │  ├ MGW xN    │        │  ├ MGW xN    │
         │  │ （多AZ）    │        │  │ （多AZ）    │        │  │ （多AZ）    │
         │  └ Redis     │◄───────│  └ Redis     │◄───────│  └ Redis     │
         │   （从）      │ 异步复制 │   （主）      │ 异步复制 │   （从）      │
         └──────────────┘        └──────────────┘        └──────────────┘

  数据面（媒体流）：Client ↔ MGW 直连（WebRTC / UDP，仅区域内）
  控制面：           Client → GW → RS/SCH → Redis（状态）、Redis pub/sub
  跨区域链路：       仅 Redis 复制（小状态，每秒 KB 级）
```

### 部署图

最小可用的三区域部署（9 台机器，每节点 Docker Compose）：

```
华东(上海)                        华北(北京)                        华南(广州)
┌────────────────────┐        ┌────────────────────┐        ┌────────────────────┐
│ sh-node-1          │        │ bj-node-1          │        │ gz-node-1          │
│  nginx (LB)        │        │  nginx (LB)        │        │  nginx (LB)        │
│  signal-gw         │        │  signal-gw         │        │  signal-gw         │
│  room-svc          │        │  room-svc          │        │  room-svc          │
│  scheduler         │        │  scheduler         │        │  scheduler         │
│  redis-sentinel    │        │  redis-sentinel    │        │  redis-sentinel    │
├────────────────────┤        ├────────────────────┤        ├────────────────────┤
│ sh-node-2          │        │ bj-node-2          │        │ gz-node-2          │
│  media-gateway x2  │        │  media-gateway x2  │        │  media-gateway x2  │
│  （UDP 40000-49999）│        │  （UDP 40000-49999）│        │  （UDP 40000-49999）│
├────────────────────┤        ├────────────────────┤        ├────────────────────┤
│ sh-node-3          │        │ bj-node-3          │        │ gz-node-3          │
│  Redis（从）        │        │  Redis（主）        │        │  Redis（从）        │
└────────────────────┘        └────────────────────┘        └────────────────────┘
         ▲                            ▲                            ▲
         └──────────── GSLB rtc.example.com ────────────────────────┘
              地域 DNS + 健康检查，故障时切换到下一个区域

扩容路径：
  - sh-node-1/bj-node-1/gz-node-1 → 拆分为 LB x2、GW x2、RS x2、SCH x2（独立机器）
  - sh-node-2/bj-node-2/gz-node-2 → 用户增长时增加更多 MGW 机器
  - Redis：每区域增加本地从库做区域级高可用（共 6 个 Redis 实例）
```

### 加入房间流程（时序图）

```
Client      GSLB       LB         GW         RS        Redis      SCH        MGW
  │           │          │          │          │          │          │          │
  │──DNS───→ │          │          │          │          │          │          │
  │←─区域IP───│         │          │          │          │          │          │
  │──WS──────────────→│─────────→ GW      │          │          │          │
  │  （JWT）    │          │          │          │          │          │          │
  │──join────→│          │─────────→ GW      │          │          │          │
  │           │          │          │──join room───────→ RS       │          │
  │           │          │          │          │──GET/SET room────→ Redis     │
  │           │          │          │          │←─房间数据────── Redis      │
  │           │          │          │←─peers,micMode──── RS       │          │
  │           │          │          │──dispatch(room,region)───────────→ SCH    │
  │           │          │          │          │          │←─查节点表── Redis
  │           │          │          │          │          │──选最优MGW───→ SCH
  │           │          │          │←─{host,port}─────────────────── SCH      │
  │←─join_ack + sfu_info─│─────────→ GW      │          │          │          │
  │──WS直连──────────────────────────────────────────────────────────────────→ MGW
  │  （createTransport / produce / consume，之后媒体流直连）
  │←══════════════════════ 媒体流（DTLS/SRTP over UDP） ══════════════════════ MGW
```

### 故障转移流程：单台 MGW 挂

```
MGW-1(挂)     SCH         Redis        GW         客户端们
  │ × ×        │            │           │            │
  │            │←─心跳停止 10s           │            │
  │            │──SET node status=DOWN─→ Redis      │
  │            │──PUBLISH node-down────→ Redis      │
  │            │            │──通知订阅者──────────→ GW（所有区域）
  │            │            │           │──sfu-failover 消息──→ 房间内所有客户端
  │            │            │           │            │
  │            │<─dispatch(room,region)─│           │
  │            │──选 MGW-2（MGW-1 已 DOWN）───→ Redis
  │            │──返回 {MGW-2.host}────────────────────│
  │            │            │←─SET room:{id}:sfu=MGW-2─ Redis
  │            │            │           │──sfu_info──→ 客户端
  │            │            │           │            │──重连 MGW-2
  │            │            │           │            │──重新 createTransport/produce/consume
  │            │            │           │            │←媒体恢复
  │
  时间线：t=0 崩溃 → t=10s 检测到 → t=10.5s 广播 → t=13s 所有客户端恢复
```

### 故障转移流程：整个区域挂

```
客户端们（在区域A）    GSLB       LB-A(挂)   Redis(failover)  LB-B        GW-B       MGW-B
  │                     │           │            │              │           │          │
  │──WS（到A区）────────→│───→ × ×  │            │              │           │          │
  │  （连接丢失）         │           │            │              │           │          │
  │                     │←─健康检查失败（10s）      │              │           │          │
  │                     │──从 DNS 摘除 A 区        │              │           │          │
  │──DNS 重新查询──────→│           │            │              │           │          │
  │←─区域B IP───────────│          │            │              │           │          │
  │──WS────────────────────────────────────────────────────────→ LB-B─────→ GW-B    │
  │──join(room-123)─────────────────────────────────────────────→ GW-B             │
  │                     │           │            │              │──GET room──→ Redis
  │                     │           │            │←─房间数据（从副本/提升后的主）─
  │                     │           │            │              │←─房间数据─ Redis
  │                     │           │            │              │──dispatch──→ SCH-B
  │                     │           │            │              │←─{MGW-B}─── SCH-B
  │←─join_ack + sfu_info(B区域)──│           │              │                       │
  │──WS───────────────────────────────────────────────────────────────────────────→ MGW-B
  │←媒体恢复──────────────────────────────────────────────────────────────────── MGW-B

  时间线：t=0 区域崩溃 → t=10-15s GSLB 摘除 → t=20-45s 客户端拿到新 DNS
          → t=25-50s 重连到区域B → t=30-60s 完全恢复
```
