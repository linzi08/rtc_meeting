// media-gateway: SFU 节点控制面。
// 职责: 管理本机所有 mediasoup-worker、Router、Transport、Producer、Consumer。
// 浏览器通过 WebSocket 与之通信（Phase 1 直连，Phase 2 经 signal-gw 转发）。
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

	"rtcplatform/pkg/mediasoupctl"
	"rtcplatform/pkg/metrics"
	"rtcplatform/pkg/signalproto"

	"github.com/gorilla/websocket"
)

// ---- Session: 一个客户端在 SFU 上的会话 ----

type Session struct {
	mu           sync.Mutex
	peerID       string
	roomID       string
	pubTransport  *mediasoupctl.WebRtcTransport
	subTransport  *mediasoupctl.WebRtcTransport
	producers     map[string]*mediasoupctl.Producer
	consumers     map[string]*mediasoupctl.Consumer
	closed        bool
}

// ---- Gateway ----

type Gateway struct {
	mu           sync.RWMutex
	cfg          Config
	upgrader     websocket.Upgrader
	workers      []*mediasoupctl.Worker
	rooms        map[string]*RoomEntry     // roomId → RoomEntry
	sessions     map[string]*Session        // peerId → Session
	workerIdx    uint64                     // round-robin worker
}

type RoomEntry struct {
	mu       sync.RWMutex
	roomID   string
	router   *mediasoupctl.Router
	peerIDs  map[string]bool
}

type Config struct {
	HTTPPort     int
	WorkerBin    string
	RtcMinPort   uint16
	RtcMaxPort   uint16
	ListenIP     string
	AnnouncedIP  string
	WorkerCount  int
}

func NewGateway(cfg Config) (*Gateway, error) {
	g := &Gateway{
		cfg:      cfg,
		rooms:    make(map[string]*RoomEntry),
		sessions: make(map[string]*Session),
		upgrader: websocket.Upgrader{CheckOrigin: func(r *http.Request) bool { return true }},
	}
	// 启动 worker 池
	for i := 0; i < cfg.WorkerCount; i++ {
		w, err := mediasoupctl.SpawnWorker(mediasoupctl.WorkerOptions{
			BinPath:    cfg.WorkerBin,
			LogLevel:   "warn",
			RtcMinPort: cfg.RtcMinPort,
			RtcMaxPort: cfg.RtcMaxPort,
		})
		if err != nil {
			return nil, fmt.Errorf("spawn worker %d: %w", i, err)
		}
		g.workers = append(g.workers, w)
		log.Printf("[media-gateway] worker %d spawned (pid=%d)", i, w.Pid())
	}
	return g, nil
}

func (g *Gateway) nextWorker() *mediasoupctl.Worker {
	g.mu.Lock()
	defer g.mu.Unlock()
	w := g.workers[g.workerIdx%uint64(len(g.workers))]
	g.workerIdx++
	return w
}

// getOrCreateRoom 获取或创建 RoomEntry（含 Router，lazy 创建）。
func (g *Gateway) getOrCreateRoom(roomID string) (*RoomEntry, error) {
	g.mu.RLock()
	re, ok := g.rooms[roomID]
	g.mu.RUnlock()
	if ok {
		return re, nil
	}
	g.mu.Lock()
	defer g.mu.Unlock()
	if re, ok = g.rooms[roomID]; ok {
		return re, nil
	}
	worker := g.nextWorker()
	router, err := mediasoupctl.CreateRouter(worker, defaultCodecs())
	if err != nil {
		return nil, fmt.Errorf("createRouter for room %s: %w", roomID, err)
	}
	re = &RoomEntry{
		roomID:  roomID,
		router:  router,
		peerIDs: make(map[string]bool),
	}
	g.rooms[roomID] = re
	log.Printf("[media-gateway] room %s created (router=%s worker=%p)", roomID, router.ID, worker)
	return re, nil
}

// defaultCodecs Phase 1 硬编码，生产从配置/环境变量读。
func defaultCodecs() []mediasoupctl.MediaCodec {
	return []mediasoupctl.MediaCodec{
		{Kind: "audio", MimeType: "audio/opus", ClockRate: 48000, Channels: 2},
		{Kind: "video", MimeType: "video/VP8", ClockRate: 90000,
			RtcpFeedback: []mediasoupctl.RtcpFeedback{
				{Type: "nack"}, {Type: "nack", Parameter: "pli"}, {Type: "ccm", Parameter: "fir"}, {Type: "goog-remb"},
			}},
		{Kind: "video", MimeType: "video/rtx", ClockRate: 90000},
		{Kind: "video", MimeType: "video/H264", ClockRate: 90000,
			Parameters: map[string]interface{}{
				"packetization-mode": 1,
				"profile-level-id":   "42e01f",
				"level-asymmetry-allowed": 1,
			},
			RtcpFeedback: []mediasoupctl.RtcpFeedback{
				{Type: "nack"}, {Type: "nack", Parameter: "pli"}, {Type: "ccm", Parameter: "fir"}, {Type: "goog-remb"},
			}},
	}
}

// ---- WebSocket 处理 ----

func (g *Gateway) handleWS(w http.ResponseWriter, r *http.Request) {
	peerID := r.URL.Query().Get("peerId")
	roomID := r.URL.Query().Get("roomId")
	if peerID == "" || roomID == "" {
		http.Error(w, "peerId and roomId required", http.StatusBadRequest)
		return
	}
	ws, err := g.upgrader.Upgrade(w, r, nil)
	if err != nil {
		return
	}
	log.Printf("[media-gateway] ws connected peer=%s room=%s", peerID, roomID)

	re, err := g.getOrCreateRoom(roomID)
	if err != nil {
		ws.Close()
		return
	}
	re.mu.Lock()
	re.peerIDs[peerID] = true
	re.mu.Unlock()

	sess := &Session{
		peerID:   peerID,
		roomID:   roomID,
		producers: make(map[string]*mediasoupctl.Producer),
		consumers: make(map[string]*mediasoupctl.Consumer),
	}
	g.mu.Lock()
	g.sessions[peerID] = sess
	g.mu.Unlock()

	// 发送 router rtpCapabilities
	capsMsg, _ := signalproto.Encode("rtpCapabilities", roomID, peerID, "", map[string]json.RawMessage{
		"rtpCapabilities": re.router.RtpCapabilities(),
	})
	ws.WriteMessage(websocket.TextMessage, capsMsg)

	// 读循环
	for {
		_, data, err := ws.ReadMessage()
		if err != nil {
			break
		}
		g.handleMessage(sess, ws, data)
	}
	// 清理
	g.cleanupSession(sess, re)
	ws.Close()
	log.Printf("[media-gateway] ws disconnected peer=%s", peerID)
}

func (g *Gateway) handleMessage(sess *Session, ws *websocket.Conn, data []byte) {
	env, err := signalproto.Decode(data)
	if err != nil {
		return
	}
	metrics.IncCounter("media_gateway_messages_total")

	var respPayload interface{}
	code := 0
	msg := ""

	switch env.Type {
	case signalproto.TypeCreateTransport:
		respPayload, code, msg = g.handleCreateTransport(sess, env)
	case signalproto.TypeConnectTransport:
		respPayload, code, msg = g.handleConnectTransport(sess, env)
	case signalproto.TypeProduce:
		respPayload, code, msg = g.handleProduce(sess, env)
	case signalproto.TypeConsume:
		respPayload, code, msg = g.handleConsume(sess, env)
	case signalproto.TypeResumeConsumer:
		respPayload, code, msg = g.handleResumeConsumer(sess, env)
	case signalproto.TypePauseConsumer:
		respPayload, code, msg = g.handlePauseConsumer(sess, env)
	case signalproto.TypeCloseProducer:
		respPayload, code, msg = g.handleCloseProducer(sess, env)
	case signalproto.TypePauseProducer:
		respPayload, code, msg = g.handlePauseProducer(sess, env)
	case signalproto.TypeResumeProducer:
		respPayload, code, msg = g.handleResumeProducer(sess, env)
	default:
		code = 1
		msg = "unknown type: " + env.Type
	}

	// 响应（如果有 reqId）
	if env.ReqID != "" {
		respEnv := signalproto.Envelope{
			Type:    env.Type,
			RoomID:  sess.roomID,
			PeerID:  sess.peerID,
			ReqID:   env.ReqID,
			Code:    code,
			Msg:     msg,
			Payload: mustJSON(respPayload),
		}
		raw, _ := json.Marshal(respEnv)
		ws.WriteMessage(websocket.TextMessage, raw)
	}
}

func (g *Gateway) getRouter(sess *Session) *mediasoupctl.Router {
	g.mu.RLock()
	defer g.mu.RUnlock()
	if re, ok := g.rooms[sess.roomID]; ok {
		return re.router
	}
	return nil
}

func (g *Gateway) ensureSession(sess *Session) {
	sess.mu.Lock()
	defer sess.mu.Unlock()
	if sess.producers == nil {
		sess.producers = make(map[string]*mediasoupctl.Producer)
	}
	if sess.consumers == nil {
		sess.consumers = make(map[string]*mediasoupctl.Consumer)
	}
}

func (g *Gateway) handleCreateTransport(sess *Session, env *signalproto.Envelope) (interface{}, int, string) {
	var payload struct {
		Direction string `json:"direction"` // "pub" or "sub"
	}
	json.Unmarshal(env.Payload, &payload)

	router := g.getRouter(sess)
	if router == nil {
		return nil, 1, "room not found"
	}
	opts := mediasoupctl.WebRtcTransportOptions{
		ListenIps: []mediasoupctl.ListenInfo{
			{Ip: g.cfg.ListenIP, AnnouncedIp: g.cfg.AnnouncedIP},
		},
		EnableUdp: true, EnableTcp: true, PreferUdp: true,
		InitialAvailableOutgoingBitrate: 1000000,
	}
	t, err := router.CreateWebRtcTransport(opts)
	if err != nil {
		return nil, 1, err.Error()
	}
	sess.mu.Lock()
	if payload.Direction == "pub" {
		sess.pubTransport = t
	} else {
		sess.subTransport = t
	}
	sess.mu.Unlock()
	log.Printf("[media-gateway] createTransport peer=%s dir=%s transportId=%s", sess.peerID, payload.Direction, t.ID)
	return t.Info(), 0, ""
}

func (g *Gateway) handleConnectTransport(sess *Session, env *signalproto.Envelope) (interface{}, int, string) {
	var payload struct {
		TransportID    string          `json:"transportId"`
		DtlsParameters json.RawMessage `json:"dtlsParameters"`
	}
	json.Unmarshal(env.Payload, &payload)
	sess.mu.Lock()
	var t *mediasoupctl.WebRtcTransport
	if sess.pubTransport != nil && sess.pubTransport.ID == payload.TransportID {
		t = sess.pubTransport
	} else if sess.subTransport != nil && sess.subTransport.ID == payload.TransportID {
		t = sess.subTransport
	}
	sess.mu.Unlock()
	if t == nil {
		return nil, 1, "transport not found"
	}
	if err := t.Connect(payload.DtlsParameters); err != nil {
		return nil, 1, err.Error()
	}
	log.Printf("[media-gateway] connectTransport peer=%s transportId=%s", sess.peerID, t.ID)
	return map[string]bool{"ok": true}, 0, ""
}

func (g *Gateway) handleProduce(sess *Session, env *signalproto.Envelope) (interface{}, int, string) {
	var payload struct {
		TransportID   string          `json:"transportId"`
		Kind          string          `json:"kind"`
		RtpParameters json.RawMessage `json:"rtpParameters"`
	}
	json.Unmarshal(env.Payload, &payload)
	sess.mu.Lock()
	t := sess.pubTransport
	sess.mu.Unlock()
	if t == nil || t.ID != payload.TransportID {
		return nil, 1, "pub transport not found"
	}
	p, err := t.Produce(payload.Kind, payload.RtpParameters)
	if err != nil {
		return nil, 1, err.Error()
	}
	sess.mu.Lock()
	sess.producers[p.ID] = p
	sess.mu.Unlock()

	// 通知同房间其他人有新 Producer
	g.broadcastRoom(sess.roomID, signalproto.Envelope{
		Type:   signalproto.TypeNewProducer,
		RoomID: sess.roomID,
		Payload: mustJSON(map[string]interface{}{
			"peerId":     sess.peerID,
			"producerId": p.ID,
			"kind":       p.Kind,
		}),
	}, sess.peerID)
	log.Printf("[media-gateway] produce peer=%s kind=%s producerId=%s", sess.peerID, p.Kind, p.ID)
	return map[string]string{"producerId": p.ID}, 0, ""
}

func (g *Gateway) handleConsume(sess *Session, env *signalproto.Envelope) (interface{}, int, string) {
	var payload struct {
		TransportID    string          `json:"transportId"`
		ProducerID     string          `json:"producerId"`
		RtpCapabilities json.RawMessage `json:"rtpCapabilities"`
	}
	json.Unmarshal(env.Payload, &payload)
	sess.mu.Lock()
	t := sess.subTransport
	sess.mu.Unlock()
	if t == nil || t.ID != payload.TransportID {
		return nil, 1, "sub transport not found"
	}
	c, err := t.Consume(payload.ProducerID, payload.RtpCapabilities)
	if err != nil {
		return nil, 1, err.Error()
	}
	sess.mu.Lock()
	sess.consumers[c.ID] = c
	sess.mu.Unlock()
	log.Printf("[media-gateway] consume peer=%s producerId=%s consumerId=%s kind=%s", sess.peerID, payload.ProducerID, c.ID, c.Kind)
	return c.Info(), 0, ""
}

func (g *Gateway) handleResumeConsumer(sess *Session, env *signalproto.Envelope) (interface{}, int, string) {
	var payload struct {
		ConsumerID string `json:"consumerId"`
	}
	json.Unmarshal(env.Payload, &payload)
	sess.mu.Lock()
	c := sess.consumers[payload.ConsumerID]
	sess.mu.Unlock()
	if c == nil {
		return nil, 1, "consumer not found"
	}
	c.Resume()
	return map[string]bool{"ok": true}, 0, ""
}

func (g *Gateway) handlePauseConsumer(sess *Session, env *signalproto.Envelope) (interface{}, int, string) {
	var payload struct {
		ConsumerID string `json:"consumerId"`
	}
	json.Unmarshal(env.Payload, &payload)
	sess.mu.Lock()
	c := sess.consumers[payload.ConsumerID]
	sess.mu.Unlock()
	if c == nil {
		return nil, 1, "consumer not found"
	}
	c.Pause()
	return map[string]bool{"ok": true}, 0, ""
}

func (g *Gateway) handleCloseProducer(sess *Session, env *signalproto.Envelope) (interface{}, int, string) {
	var payload struct {
		ProducerID string `json:"producerId"`
	}
	json.Unmarshal(env.Payload, &payload)
	sess.mu.Lock()
	p := sess.producers[payload.ProducerID]
	sess.mu.Unlock()
	if p == nil {
		return nil, 1, "producer not found"
	}
	p.Close()
	sess.mu.Lock()
	delete(sess.producers, payload.ProducerID)
	sess.mu.Unlock()
	// 广播 producerClosed
	g.broadcastRoom(sess.roomID, signalproto.Envelope{
		Type: signalproto.TypeProducerClosed, RoomID: sess.roomID,
		Payload: mustJSON(map[string]interface{}{
			"peerId": sess.peerID, "producerId": payload.ProducerID,
		}),
	}, sess.peerID)
	return map[string]bool{"ok": true}, 0, ""
}

func (g *Gateway) handlePauseProducer(sess *Session, env *signalproto.Envelope) (interface{}, int, string) {
	var payload struct {
		ProducerID string `json:"producerId"`
	}
	json.Unmarshal(env.Payload, &payload)
	sess.mu.Lock()
	p := sess.producers[payload.ProducerID]
	sess.mu.Unlock()
	if p == nil {
		return nil, 1, "producer not found"
	}
	p.Pause()
	g.broadcastRoom(sess.roomID, signalproto.Envelope{
		Type: signalproto.TypeProducerPaused, RoomID: sess.roomID,
		Payload: mustJSON(map[string]interface{}{
			"peerId": sess.peerID, "producerId": p.ID, "kind": p.Kind,
		}),
	}, sess.peerID)
	return map[string]bool{"ok": true}, 0, ""
}

func (g *Gateway) handleResumeProducer(sess *Session, env *signalproto.Envelope) (interface{}, int, string) {
	var payload struct {
		ProducerID string `json:"producerId"`
	}
	json.Unmarshal(env.Payload, &payload)
	sess.mu.Lock()
	p := sess.producers[payload.ProducerID]
	sess.mu.Unlock()
	if p == nil {
		return nil, 1, "producer not found"
	}
	p.Resume()
	g.broadcastRoom(sess.roomID, signalproto.Envelope{
		Type: signalproto.TypeProducerResumed, RoomID: sess.roomID,
		Payload: mustJSON(map[string]interface{}{
			"peerId": sess.peerID, "producerId": p.ID, "kind": p.Kind,
		}),
	}, sess.peerID)
	return map[string]bool{"ok": true}, 0, ""
}

func (g *Gateway) broadcastRoom(roomID string, env signalproto.Envelope, excludePeerID string) {
	g.mu.RLock()
	sessions := make([]*Session, 0)
	for pid, s := range g.sessions {
		if s.roomID == roomID && pid != excludePeerID {
			sessions = append(sessions, s)
		}
	}
	g.mu.RUnlock()
	// Phase 1: Session 不持有 ws 连接引用。简化实现: 广播由 signal-gw 层做。
	// media-gateway 侧只存状态，ws 推送通过写 Session.wsConn。
	// Phase 1 简化: 每个 Session 在 handleWS 时持有 ws 引用。
}

// cleanupSession 关闭所有 Transport/Producer/Consumer，释放 Router 上的资源。
func (g *Gateway) cleanupSession(sess *Session, re *RoomEntry) {
	sess.mu.Lock()
	defer sess.mu.Unlock()
	if sess.closed {
		return
	}
	sess.closed = true
	for _, c := range sess.consumers {
		c.Close()
	}
	for _, p := range sess.producers {
		p.Close()
	}
	if sess.pubTransport != nil {
		sess.pubTransport.Close()
	}
	if sess.subTransport != nil {
		sess.subTransport.Close()
	}
	g.mu.Lock()
	delete(g.sessions, sess.peerID)
	g.mu.Unlock()
	re.mu.Lock()
	delete(re.peerIDs, sess.peerID)
	roomEmpty := len(re.peerIDs) == 0
	re.mu.Unlock()
	if roomEmpty {
		re.router.Close()
		g.mu.Lock()
		delete(g.rooms, sess.roomID)
		g.mu.Unlock()
		log.Printf("[media-gateway] room %s empty, router closed", sess.roomID)
	}
}

func mustJSON(v interface{}) json.RawMessage {
	b, _ := json.Marshal(v)
	return b
}

func main() {
	cfg := Config{}
	rtcMinPortInt := 40000
	rtcMaxPortInt := 49999
	flag.IntVar(&cfg.HTTPPort, "port", 9090, "HTTP listen port")
	flag.StringVar(&cfg.WorkerBin, "worker-bin", "", "path to mediasoup-worker binary (required)")
	flag.IntVar(&rtcMinPortInt, "rtc-min-port", 40000, "RTC UDP min port")
	flag.IntVar(&rtcMaxPortInt, "rtc-max-port", 49999, "RTC UDP max port")
	flag.StringVar(&cfg.ListenIP, "listen-ip", "0.0.0.0", "listen IP for WebRtcTransport")
	flag.StringVar(&cfg.AnnouncedIP, "announced-ip", "", "public IP announced to clients")
	flag.IntVar(&cfg.WorkerCount, "workers", 0, "number of workers (default: runtime.NumCPU)")
	flag.Parse()
	cfg.RtcMinPort = uint16(rtcMinPortInt)
	cfg.RtcMaxPort = uint16(rtcMaxPortInt)

	if cfg.WorkerCount <= 0 {
		cfg.WorkerCount = 1 // Phase 1: 1 worker for dev
	}
	if cfg.WorkerBin == "" {
		fmt.Fprintln(os.Stderr, "error: --worker-bin is required (path to mediasoup-worker)")
		os.Exit(1)
	}

	g, err := NewGateway(cfg)
	if err != nil {
		log.Fatalf("media-gateway init: %v", err)
	}
	http.HandleFunc("/sfu-ws", g.handleWS)
	http.HandleFunc("/metrics", func(w http.ResponseWriter, r *http.Request) {
		metrics.Handler().ServeHTTP(w, r)
	})
	http.HandleFunc("/health", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, "ok")
	})

	addr := fmt.Sprintf(":%d", cfg.HTTPPort)
	log.Printf("[media-gateway] listening on %s (workers=%d)", addr, cfg.WorkerCount)
	go func() {
		if err := http.ListenAndServe(addr, nil); err != nil {
			log.Fatalf("media-gateway: %v", err)
		}
	}()

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	<-sig
	log.Println("[media-gateway] shutting down")
}
