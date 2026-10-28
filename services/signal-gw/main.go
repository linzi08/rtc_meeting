// signal-gw: 信令网关。
// 职责: WebSocket 长连接接入、JWT 鉴权、消息路由、房间广播。
// 不含业务逻辑；所有房间/连麦决策由 room-svc 决定，这里只做消息转发。
//
// Phase 1 单节点模式: signal-gw 内嵌 room-svc 逻辑（通过 RoomManager 间接调用）。
// Phase 2 拆分: signal-gw 与 room-svc 之间走内部 RPC。
package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"net/http"
	"os"
	"os/signal"
	"sync"
	"syscall"
	"time"

	"rtcplatform/pkg/jwtutil"
	"rtcplatform/pkg/metrics"
	"rtcplatform/pkg/signalproto"

	"github.com/gorilla/websocket"
)

// ---- 配置 ----

type Config struct {
	HTTPPort    int
	JWTSecret   string
	SFUWSHost   string // media-gateway WS 地址（Phase 1 内嵌则为空）
	PublicDir   string
}

// ---- Peer 连接 ----

type PeerConn struct {
	mu       sync.Mutex
	conn     *websocket.Conn
	peerID   string
	roomID   string
	role     string
	closed   bool
	sendMu   sync.Mutex
}

func (p *PeerConn) Send(env signalproto.Envelope) error {
	raw, err := json.Marshal(env)
	if err != nil {
		return err
	}
	p.sendMu.Lock()
	defer p.sendMu.Unlock()
	return p.conn.WriteMessage(websocket.TextMessage, raw)
}

// ---- Hub: 连接管理 + 房间路由 ----

type Hub struct {
	mu    sync.RWMutex
	rooms map[string]map[string]*PeerConn // roomID → peerID → conn
	// reqId 去重（5s 窗口）
	reqCache map[string]time.Time
}

func NewHub() *Hub {
	h := &Hub{
		rooms:    make(map[string]map[string]*PeerConn),
		reqCache: make(map[string]time.Time),
	}
	go h.gcReqCache()
	return h
}

func (h *Hub) gcReqCache() {
	ticker := time.NewTicker(30 * time.Second)
	for range ticker.C {
		h.mu.Lock()
		cut := time.Now().Add(-10 * time.Second)
		for k, ts := range h.reqCache {
			if ts.Before(cut) {
				delete(h.reqCache, k)
			}
		}
		h.mu.Unlock()
	}
}

func (h *Hub) JoinRoom(roomID, peerID string, pc *PeerConn) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.rooms[roomID] == nil {
		h.rooms[roomID] = make(map[string]*PeerConn)
	}
	h.rooms[roomID][peerID] = pc
}

func (h *Hub) LeaveRoom(roomID, peerID string) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.rooms[roomID] != nil {
		delete(h.rooms[roomID], peerID)
		if len(h.rooms[roomID]) == 0 {
			delete(h.rooms, roomID)
		}
	}
}

func (h *Hub) Broadcast(roomID string, env signalproto.Envelope, excludePeerID string) {
	h.mu.RLock()
	conns := make([]*PeerConn, 0, len(h.rooms[roomID]))
	for pid, pc := range h.rooms[roomID] {
		if pid != excludePeerID {
			conns = append(conns, pc)
		}
	}
	h.mu.RUnlock()
	for _, pc := range conns {
		if err := pc.Send(env); err != nil {
			// 发送失败不阻塞其他 peer
		}
	}
}

func (h *Hub) IsDuplicateReqID(reqID string) bool {
	h.mu.Lock()
	defer h.mu.Unlock()
	if _, ok := h.reqCache[reqID]; ok {
		return true
	}
	h.reqCache[reqID] = time.Now()
	return false
}

// ---- RoomState: 房间业务状态（Phase 1 内嵌 room-svc 简化版） ----

type PeerInfo struct {
	PeerID      string `json:"peerId"`
	DisplayName string `json:"displayName"`
	Role        string `json:"role"` // host | guest | audience
	State       string `json:"state"` // JOINED | ON_STAGE
	Producers   []ProducerInfo `json:"producers"`
}

type ProducerInfo struct {
	ProducerID string `json:"producerId"`
	PeerID     string `json:"peerId"`
	Kind       string `json:"kind"`
	Paused     bool   `json:"paused"`
}

type RoomState struct {
	mu        sync.RWMutex
	roomID    string
	peers     map[string]*PeerInfo
	rtpCaps   json.RawMessage // 从 media-gateway 拿到，缓存
	micQueue  []string
	micMode   string // free | apply | hostOnly
}

func NewRoomState(roomID string) *RoomState {
	return &RoomState{
		roomID:  roomID,
		peers:   make(map[string]*PeerInfo),
		micMode: "free",
	}
}

func (rs *RoomState) AddPeer(p *PeerInfo) {
	rs.mu.Lock()
	defer rs.mu.Unlock()
	rs.peers[p.PeerID] = p
}

func (rs *RoomState) RemovePeer(peerID string) {
	rs.mu.Lock()
	defer rs.mu.Unlock()
	delete(rs.peers, peerID)
	for i, q := range rs.micQueue {
		if q == peerID {
			rs.micQueue = append(rs.micQueue[:i], rs.micQueue[i+1:]...)
			break
		}
	}
}

func (rs *RoomState) ListPeers() []PeerInfo {
	rs.mu.RLock()
	defer rs.mu.RUnlock()
	out := make([]PeerInfo, 0, len(rs.peers))
	for _, p := range rs.peers {
		out = append(out, *p)
	}
	return out
}

func (rs *RoomState) GetPeer(peerID string) *PeerInfo {
	rs.mu.RLock()
	defer rs.mu.RUnlock()
	if p, ok := rs.peers[peerID]; ok {
		cp := *p
		return &cp
	}
	return nil
}

func (rs *RoomState) SetRtpCaps(caps json.RawMessage) {
	rs.mu.Lock()
	rs.rtpCaps = caps
	rs.mu.Unlock()
}

func (rs *RoomState) RtpCaps() json.RawMessage {
	rs.mu.RLock()
	defer rs.mu.RUnlock()
	return rs.rtpCaps
}

func (rs *RoomState) AddProducer(peerID, producerID, kind string) {
	rs.mu.Lock()
	defer rs.mu.Unlock()
	if p, ok := rs.peers[peerID]; ok {
		p.Producers = append(p.Producers, ProducerInfo{
			ProducerID: producerID, PeerID: peerID, Kind: kind,
		})
		if p.State == "JOINED" && kind == "audio" {
			p.State = "ON_STAGE"
		}
	}
}

func (rs *RoomState) RemoveProducer(producerID string) {
	rs.mu.Lock()
	defer rs.mu.Unlock()
	for _, p := range rs.peers {
		for i, prod := range p.Producers {
			if prod.ProducerID == producerID {
				p.Producers = append(p.Producers[:i], p.Producers[i+1:]...)
				return
			}
		}
	}
}

// ---- SignalGateway 主服务 ----

type Gateway struct {
	cfg     Config
	hub     *Hub
	rooms   map[string]*RoomState
	roomMu  sync.RWMutex
	upgrader websocket.Upgrader
}

func NewGateway(cfg Config) *Gateway {
	return &Gateway{
		cfg:   cfg,
		hub:   NewHub(),
		rooms: make(map[string]*RoomState),
		upgrader: websocket.Upgrader{
			CheckOrigin: func(r *http.Request) bool { return true }, // 生产应校验 Origin
		},
	}
}

func (g *Gateway) getRoom(roomID string) *RoomState {
	g.roomMu.Lock()
	defer g.roomMu.Unlock()
	if rs, ok := g.rooms[roomID]; ok {
		return rs
	}
	rs := NewRoomState(roomID)
	g.rooms[roomID] = rs
	return rs
}

func (g *Gateway) handleWS(w http.ResponseWriter, r *http.Request) {
	// JWT 鉴权: URL query ?token=<jwt>
	token := r.URL.Query().Get("token")
	claims, err := jwtutil.Verify(g.cfg.JWTSecret, token)
	if err != nil {
		http.Error(w, "unauthorized: "+err.Error(), http.StatusUnauthorized)
		return
	}

	ws, err := g.upgrader.Upgrade(w, r, nil)
	if err != nil {
		log.Printf("[signal-gw] ws upgrade: %v", err)
		return
	}
	peerID := claims.PeerID
	if peerID == "" {
		// 服务端分配
		peerID = fmt.Sprintf("p-%d", time.Now().UnixNano())
	}
	pc := &PeerConn{conn: ws, peerID: peerID, role: claims.Role}
	metrics.IncCounter("signal_gw_connections_total")
	metrics.SetGauge("signal_gw_active_connections", float64(g.activeCount()+1))

	log.Printf("[signal-gw] connected peer=%s room=%s", peerID, claims.RoomID)

	// 读循环
	for {
		_, data, err := ws.ReadMessage()
		if err != nil {
			break
		}
		g.handleMessage(pc, data)
	}
	// 清理
	if pc.roomID != "" {
		g.hub.LeaveRoom(pc.roomID, pc.peerID)
		rs := g.getRoom(pc.roomID)
		rs.RemoveProducerAllByPeer(pc.peerID)
		rs.RemovePeer(pc.peerID)
		g.hub.Broadcast(pc.roomID, signalproto.Envelope{
			Type: signalproto.TypePeerLeft, RoomID: pc.roomID, PeerID: pc.peerID,
			Payload: mustJSON(map[string]string{"peerId": pc.peerID}),
		}, "")
	}
	pc.mu.Lock()
	pc.closed = true
	pc.mu.Unlock()
	ws.Close()
	metrics.SetGauge("signal_gw_active_connections", float64(g.activeCount()))
	log.Printf("[signal-gw] disconnected peer=%s", pc.peerID)
}

func (g *Gateway) activeCount() int {
	g.hub.mu.RLock()
	defer g.hub.mu.RUnlock()
	n := 0
	for _, room := range g.hub.rooms {
		n += len(room)
	}
	return n
}

func (g *Gateway) handleMessage(pc *PeerConn, data []byte) {
	env, err := signalproto.Decode(data)
	if err != nil {
		log.Printf("[signal-gw] decode err: %v", err)
		return
	}
	metrics.IncCounter("signal_gw_messages_total")

	// reqId 去重
	if env.ReqID != "" && g.hub.IsDuplicateReqID(env.ReqID) {
		pc.Send(signalproto.Envelope{
			Type: env.Type, ReqID: env.ReqID, Code: 1, Msg: "duplicate reqId",
		})
		return
	}

	switch env.Type {
	case signalproto.TypeJoin:
		g.handleJoin(pc, env)
	case signalproto.TypeLeave:
		g.handleLeave(pc, env)
	case signalproto.TypePing:
		pc.Send(signalproto.Envelope{
			Type: signalproto.TypePong, ReqID: env.ReqID,
			Payload: env.Payload,
		})
	case signalproto.TypeChat:
		g.handleChat(pc, env)
	case signalproto.TypeRequestMic:
		g.handleRequestMic(pc, env)
	case signalproto.TypeGrantMic:
		g.handleGrantMic(pc, env)
	case signalproto.TypeDenyMic:
		g.handleDenyMic(pc, env)
	case signalproto.TypeKickMic:
		g.handleKickMic(pc, env)
	default:
		// createTransport/connectTransport/produce/consume 等媒体信令
		// Phase 1: 直接转发到 media-gateway（或内嵌处理）
		g.forwardToMedia(pc, env)
	}
}

func (g *Gateway) handleJoin(pc *PeerConn, env *signalproto.Envelope) {
	var payload struct {
		DisplayName string `json:"displayName"`
		Role        string `json:"role"`
	}
	json.Unmarshal(env.Payload, &payload)
	if payload.Role == "" {
		payload.Role = "audience"
	}
	if payload.DisplayName == "" {
		payload.DisplayName = pc.peerID
	}

	roomID := env.RoomID
	pc.mu.Lock()
	pc.roomID = roomID
	pc.mu.Unlock()

	rs := g.getRoom(roomID)
	peerInfo := &PeerInfo{
		PeerID:      pc.peerID,
		DisplayName: payload.DisplayName,
		Role:        payload.Role,
		State:       "JOINED",
	}
	rs.AddPeer(peerInfo)
	g.hub.JoinRoom(roomID, pc.peerID, pc)

	// 响应: 全量 roomInfo
	resp := map[string]interface{}{
		"peerId": pc.peerID,
		"peers":  rs.ListPeers(),
		"sfuWS":  g.cfg.SFUWSHost,
	}
	if rs.RtpCaps() != nil {
		resp["rtpCapabilities"] = rs.RtpCaps()
	}
	pc.Send(signalproto.Envelope{
		Type: signalproto.TypeJoin, RoomID: roomID, PeerID: pc.peerID,
		ReqID: env.ReqID, Code: 0,
		Payload: mustJSON(resp),
	})

	// 广播给其他人
	g.hub.Broadcast(roomID, signalproto.Envelope{
		Type: signalproto.TypePeerJoined, RoomID: roomID,
		Payload: mustJSON(peerInfo),
	}, pc.peerID)
	g.hub.Broadcast(roomID, signalproto.Envelope{
		Type: signalproto.TypeRoomUpdate, RoomID: roomID,
		Payload: mustJSON(map[string]interface{}{"peers": rs.ListPeers()}),
	}, "")
	log.Printf("[signal-gw] join peer=%s room=%s displayName=%s", pc.peerID, roomID, payload.DisplayName)
}

func (g *Gateway) handleLeave(pc *PeerConn, env *signalproto.Envelope) {
	if pc.roomID != "" {
		g.hub.LeaveRoom(pc.roomID, pc.peerID)
		rs := g.getRoom(pc.roomID)
		rs.RemoveProducerAllByPeer(pc.peerID)
		rs.RemovePeer(pc.peerID)
		g.hub.Broadcast(pc.roomID, signalproto.Envelope{
			Type: signalproto.TypePeerLeft, RoomID: pc.roomID, PeerID: pc.peerID,
			Payload: mustJSON(map[string]string{"peerId": pc.peerID}),
		}, "")
	}
	pc.Send(signalproto.Envelope{Type: signalproto.TypeLeave, ReqID: env.ReqID, Code: 0})
}

func (g *Gateway) handleChat(pc *PeerConn, env *signalproto.Envelope) {
	var payload struct {
		Text string `json:"text"`
	}
	json.Unmarshal(env.Payload, &payload)
	pi := g.getRoom(env.RoomID).GetPeer(pc.peerID)
	name := pc.peerID
	if pi != nil {
		name = pi.DisplayName
	}
	g.hub.Broadcast(env.RoomID, signalproto.Envelope{
		Type: signalproto.TypeChat, RoomID: env.RoomID,
		Payload: mustJSON(map[string]interface{}{
			"fromPeerId": pc.peerID, "displayName": name, "text": payload.Text, "ts": time.Now().Unix(),
		}),
	}, "")
}

func (g *Gateway) handleRequestMic(pc *PeerConn, env *signalproto.Envelope) {
	rs := g.getRoom(env.RoomID)
	rs.mu.Lock()
	rs.micQueue = append(rs.micQueue, pc.peerID)
	rs.mu.Unlock()
	// 推给所有人（简化: 所有 host + 本人; Phase 2 按 role 过滤）
	g.hub.Broadcast(env.RoomID, signalproto.Envelope{
		Type: signalproto.TypeMicRequest, RoomID: env.RoomID,
		Payload: mustJSON(map[string]string{"peerId": pc.peerID}),
	}, "")
	pc.Send(signalproto.Envelope{Type: env.Type, ReqID: env.ReqID, Code: 0})
}

func (g *Gateway) handleGrantMic(pc *PeerConn, env *signalproto.Envelope) {
	var payload struct {
		TargetPeerID string `json:"targetPeerId"`
	}
	json.Unmarshal(env.Payload, &payload)
	// Phase 1: 广播 micGranted（Phase 2 校验权限 + 点对点）
	g.hub.Broadcast(env.RoomID, signalproto.Envelope{
		Type: signalproto.TypeMicGranted, RoomID: env.RoomID,
		Payload: mustJSON(map[string]string{"targetPeerId": payload.TargetPeerID}),
	}, "")
	pc.Send(signalproto.Envelope{Type: env.Type, ReqID: env.ReqID, Code: 0})
}

func (g *Gateway) handleDenyMic(pc *PeerConn, env *signalproto.Envelope) {
	var payload struct {
		TargetPeerID string `json:"targetPeerId"`
	}
	json.Unmarshal(env.Payload, &payload)
	g.hub.Broadcast(env.RoomID, signalproto.Envelope{
		Type: signalproto.TypeMicDenied, RoomID: env.RoomID,
		Payload: mustJSON(map[string]string{"targetPeerId": payload.TargetPeerID}),
	}, "")
	pc.Send(signalproto.Envelope{Type: env.Type, ReqID: env.ReqID, Code: 0})
}

func (g *Gateway) handleKickMic(pc *PeerConn, env *signalproto.Envelope) {
	var payload struct {
		TargetPeerID string `json:"targetPeerId"`
	}
	json.Unmarshal(env.Payload, &payload)
	g.hub.Broadcast(env.RoomID, signalproto.Envelope{
		Type: signalproto.TypeMicRevoked, RoomID: env.RoomID,
		Payload: mustJSON(map[string]string{"targetPeerId": payload.TargetPeerID}),
	}, "")
	pc.Send(signalproto.Envelope{Type: env.Type, ReqID: env.ReqID, Code: 0})
}

// forwardToMedia: 媒体信令透传。Phase 1 单节点模式内嵌：Phase 2 拆到 media-gateway。
// 这里仅记录日志，实际处理在 media-gateway（浏览器同时连接两个 WS）。
func (g *Gateway) forwardToMedia(pc *PeerConn, env *signalproto.Envelope) {
	// Phase 1: 浏览器连 signal-gw 拿 roomInfo，然后直连 media-gateway 发媒体信令。
	// signal-gw 只负责业务信令（join/leave/chat/mic），不转发媒体消息。
	log.Printf("[signal-gw] non-business msg type=%s peer=%s (expected on SFU WS, ignored)", env.Type, pc.peerID)
}

// RemoveProducerAllByPeer 清除某人所有 producer 记录。
func (rs *RoomState) RemoveProducerAllByPeer(peerID string) {
	rs.mu.Lock()
	defer rs.mu.Unlock()
	if p, ok := rs.peers[peerID]; ok {
		p.Producers = nil
		if p.State == "ON_STAGE" {
			p.State = "JOINED"
		}
	}
}

func mustJSON(v interface{}) json.RawMessage {
	b, _ := json.Marshal(v)
	return b
}

func main() {
	cfg := Config{}
	flag.IntVar(&cfg.HTTPPort, "port", 8080, "HTTP listen port")
	flag.StringVar(&cfg.JWTSecret, "jwt-secret", "dev-secret", "JWT secret")
	flag.StringVar(&cfg.SFUWSHost, "sfu-ws", "ws://127.0.0.1:9090", "media-gateway WS URL for client")
	flag.StringVar(&cfg.PublicDir, "public", "", "static dir (optional)")
	flag.Parse()

	g := NewGateway(cfg)
	http.HandleFunc("/ws", g.handleWS)
	http.HandleFunc("/metrics", func(w http.ResponseWriter, r *http.Request) {
		metrics.Handler().ServeHTTP(w, r)
	})
	http.HandleFunc("/health", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, "ok")
	})
	if cfg.PublicDir != "" {
		fs := http.FileServer(http.Dir(cfg.PublicDir))
		http.Handle("/", fs)
	}

	addr := fmt.Sprintf(":%d", cfg.HTTPPort)
	log.Printf("[signal-gw] listening on %s", addr)
	go func() {
		if err := http.ListenAndServe(addr, nil); err != nil {
			log.Fatalf("signal-gw: %v", err)
		}
	}()

	// 优雅退出
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	<-sig
	log.Println("[signal-gw] shutting down")
}
