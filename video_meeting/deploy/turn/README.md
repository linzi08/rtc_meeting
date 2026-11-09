# TURN（coturn）部署指南

## 什么时候需要 TURN

- **局域网 / 同机**：不需要（ICE 直连 mediasoup-demo 服务器）。
- **跨公网**且客户端在 NAT 后：能打洞则 STUN 足够；对称 NAT/防火墙严格时必须 TURN 中继。
- TURN 是**纯客户端侧 ICE 配置**：mediasoup-demo 服务端零改动，coturn 可与 demo 同机或独立公网机。

## 一键部署（Docker）

```bash
cd deploy/turn
# 1. 改 turnserver.conf：
#    external-ip=<你的公网IP>
#    static-auth-secret=<随机长串>          # openssl rand -hex 32
# 2. （可选 turns:）Let's Encrypt 证书放 ./certs/（fullchain.pem + privkey.pem）
docker compose up -d
docker logs -f coturn        # 看到 "IPv4. CLI ... " 即启动完成
```

## 裸机部署（systemd，替代方案）

```bash
apt install -y coturn
cp turnserver.conf /etc/turnserver.conf       # 按上修改 external-ip / secret
# /etc/default/coturn: 打开 TURNSERVER_ENABLED=1
systemctl enable --now coturn
```

## 防火墙清单

| 端口 | 协议 | 用途 |
|---|---|---|
| 3478 | UDP+TCP | STUN/TURN |
| 5349 | TCP | TURN-TLS（turns:） |
| 49160–49200 | UDP | 中继端口段（min-port/max-port） |

云服务器安全组同样放行（UDP 49160-49200 最容易漏）。

## 生成时效凭据并接入客户端

```bash
node gen-credential.js <secret> 60        # 60 分钟有效
# 输出：--turn-user 1762300000 --turn-pass AbCd...

client.exe --url wss://<demo服务器>:4443 --room demo --name bob \
    --stun stun:<TURN_IP>:3478 \
    --turn turn:<TURN_IP>:3478?transport=udp \
    --turn-user 1762300000 --turn-pass AbCd...
```

严格防火墙（只放 443）变体：

```
--turn turns:<TURN_IP>:443?transport=tcp --turn-user ... --turn-pass ...
（coturn 侧：tls-listening-port=443，证书用真域名签发）
```

## 验证

1. 客户端启动日志出现 `relay` 候选（type=relay）即 TURN 生效（未用 TURN 时只有 host/srflx）。
2. 浏览器验证：https://webrtc.github.io/samples/src/content/peerconnection/trickle-ice/
   填 `turn:IP:3478` + 凭据 → Done，出现 type=relay。
3. coturn 日志：`allocate`/`relay` 条目。
