// room-svc: 房间/连麦业务服务。
// 职责: 房间状态机（JOIN→ON_STAGE）、连麦审批、权限。
// Phase 1: signal-gw 内嵌了简化版，此服务是完整版接口（Phase 2 拆分后独立部署）。
// 提供 HTTP JSON API，signal-gw 调用。
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
)

// ---- Peer / Room 业务模型 ----

type PeerState string

const (
	PeerDisconnected PeerState = "DISCONNECTED"
	PeerJoined       PeerState = "JOINED"
	PeerMicRequested PeerState = "MIC_REQUESTED"
	PeerOnStage      PeerState = "ON_STAGE"
)

type Peer struct {
	PeerID      string    `json:"peerId"`
	DisplayName string    `json:"displayName"`
	Role        string    `json:"role"` // host | guest | audience
	State       PeerState `json:"state"`
	JoinTs      int64     `json:"joinTs"`
}

type Room struct {
	mu        sync.RWMutex
	RoomID    string
	MaxMic    int
	MicMode   string // free | apply | hostOnly
	Peers     map[string]*Peer
	MicQueue  []string
	CreateTime int64
}

type RoomService struct {
	mu    sync.RWMutex
	rooms map[string]*Room
}

func NewRoomService() *RoomService {
	return &RoomService{rooms: make(map[string]*Room)}
}

func (rs *RoomService) GetRoom(id string) *Room {
	rs.mu.RLock()
	defer rs.mu.RUnlock()
	return rs.rooms[id]
}

func (rs *RoomService) CreateRoom(id string, maxMic int, micMode string) *Room {
	rs.mu.Lock()
	defer rs.mu.Unlock()
	if r, ok := rs.rooms[id]; ok {
		return r
	}
	if maxMic <= 0 { maxMic = 9 }
	if micMode == "" { micMode = "free" }
	r := &Room{
		RoomID: id, MaxMic: maxMic, MicMode: micMode,
		Peers: make(map[string]*Peer), CreateTime: time.Now().Unix(),
	}
	rs.rooms[id] = r
	return r
}

// ---- HTTP handlers ----

type APIRequest struct {
	RoomID string `json:"roomId"`
	PeerID string `json:"peerId"`
	Role   string `json:"role"`
	DisplayName string `json:"displayName"`
	TargetPeerID string `json:"targetPeerId"`
	MaxMic int    `json:"maxMic"`
	MicMode string `json:"micMode"`
}

type APIResponse struct {
	Code  int         `json:"code"`
	Msg   string      `json:"msg,omitempty"`
	Data  interface{} `json:"data,omitempty"`
}

func (rs *RoomService) handleJoin(w http.ResponseWriter, r *http.Request) {
	var req APIRequest
	json.NewDecoder(r.Body).Decode(&req)
	if req.RoomID == "" || req.PeerID == "" {
		writeJSON(w, APIResponse{Code: 1, Msg: "roomId and peerId required"})
		return
	}
	room := rs.CreateRoom(req.RoomID, req.MaxMic, req.MicMode)
	room.mu.Lock()
	room.Peers[req.PeerID] = &Peer{
		PeerID: req.PeerID, DisplayName: req.DisplayName,
		Role: orDefault(req.Role, "audience"), State: PeerJoined, JoinTs: time.Now().Unix(),
	}
	room.mu.Unlock()
	// 返回房间信息（含所有 peers）
	room.mu.RLock()
	peers := make([]Peer, 0, len(room.Peers))
	for _, p := range room.Peers { peers = append(peers, *p) }
	room.mu.RUnlock()
	writeJSON(w, APIResponse{Code: 0, Data: map[string]interface{}{"peers": peers, "micMode": room.MicMode}})
}

func (rs *RoomService) handleLeave(w http.ResponseWriter, r *http.Request) {
	var req APIRequest
	json.NewDecoder(r.Body).Decode(&req)
	room := rs.GetRoom(req.RoomID)
	if room == nil { writeJSON(w, APIResponse{Code: 0}); return }
	room.mu.Lock()
	delete(room.Peers, req.PeerID)
	for i, q := range room.MicQueue {
		if q == req.PeerID { room.MicQueue = append(room.MicQueue[:i], room.MicQueue[i+1:]...); break }
	}
	room.mu.Unlock()
	writeJSON(w, APIResponse{Code: 0})
}

func (rs *RoomService) handleRequestMic(w http.ResponseWriter, r *http.Request) {
	var req APIRequest
	json.NewDecoder(r.Body).Decode(&req)
	room := rs.GetRoom(req.RoomID)
	if room == nil { writeJSON(w, APIResponse{Code: 1, Msg: "room not found"}); return }
	room.mu.Lock()
	room.MicQueue = append(room.MicQueue, req.PeerID)
	room.mu.Unlock()
	writeJSON(w, APIResponse{Code: 0})
}

func (rs *RoomService) handleGrantMic(w http.ResponseWriter, r *http.Request) {
	var req APIRequest
	json.NewDecoder(r.Body).Decode(&req)
	room := rs.GetRoom(req.RoomID)
	if room == nil { writeJSON(w, APIResponse{Code: 1, Msg: "room not found"}); return }
	room.mu.Lock()
	defer room.mu.Unlock()
	// 移出队列
	for i, q := range room.MicQueue {
		if q == req.TargetPeerID { room.MicQueue = append(room.MicQueue[:i], room.MicQueue[i+1:]...); break }
	}
	// 检查麦位数量
	onStage := 0
	for _, p := range room.Peers { if p.State == PeerOnStage { onStage++ } }
	if onStage >= room.MaxMic {
		writeJSON(w, APIResponse{Code: 2, Msg: "mic full"})
		return
	}
	room.MicQueue = append(room.MicQueue, req.TargetPeerID)
	if p, ok := room.Peers[req.TargetPeerID]; ok { p.State = PeerOnStage }
	writeJSON(w, APIResponse{Code: 0})
}

func (rs *RoomService) handleKickMic(w http.ResponseWriter, r *http.Request) {
	var req APIRequest
	json.NewDecoder(r.Body).Decode(&req)
	room := rs.GetRoom(req.RoomID)
	if room == nil { writeJSON(w, APIResponse{Code: 1, Msg: "room not found"}); return }
	room.mu.Lock()
	if p, ok := room.Peers[req.TargetPeerID]; ok {
		if p.State == PeerOnStage { p.State = PeerJoined }
	}
	room.mu.Unlock()
	writeJSON(w, APIResponse{Code: 0})
}

func (rs *RoomService) handleRoomInfo(w http.ResponseWriter, r *http.Request) {
	roomID := r.URL.Query().Get("roomId")
	room := rs.GetRoom(roomID)
	if room == nil { writeJSON(w, APIResponse{Code: 1, Msg: "room not found"}); return }
	room.mu.RLock()
	peers := make([]Peer, 0, len(room.Peers))
	for _, p := range room.Peers { peers = append(peers, *p) }
	micQueue := make([]string, len(room.MicQueue))
	copy(micQueue, room.MicQueue)
	mode := room.MicMode
	maxMic := room.MaxMic
	room.mu.RUnlock()
	writeJSON(w, APIResponse{Code: 0, Data: map[string]interface{}{
		"roomId": roomID, "peers": peers, "micQueue": micQueue, "micMode": mode, "maxMic": maxMic,
	}})
}

func writeJSON(w http.ResponseWriter, resp APIResponse) {
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(resp)
}

func orDefault(v, def string) string { if v == "" { return def }; return v }

func main() {
	port := flag.Int("port", 8081, "HTTP listen port")
	flag.Parse()
	rs := NewRoomService()
	mux := http.NewServeMux()
	mux.HandleFunc("/api/v1/room/join", rs.handleJoin)
	mux.HandleFunc("/api/v1/room/leave", rs.handleLeave)
	mux.HandleFunc("/api/v1/room/requestMic", rs.handleRequestMic)
	mux.HandleFunc("/api/v1/room/grantMic", rs.handleGrantMic)
	mux.HandleFunc("/api/v1/room/kickMic", rs.handleKickMic)
	mux.HandleFunc("/api/v1/room/info", rs.handleRoomInfo)
	mux.HandleFunc("/health", func(w http.ResponseWriter, r *http.Request) { fmt.Fprint(w, "ok") })

	addr := fmt.Sprintf(":%d", *port)
	log.Printf("[room-svc] listening on %s", addr)
	go func() {
		if err := http.ListenAndServe(addr, mux); err != nil { log.Fatalf("room-svc: %v", err) }
	}()
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	<-sig
}
