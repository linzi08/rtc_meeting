// scheduler: SFU 节点调度器。
// 职责: 接收 media-gateway 负载上报，按评分公式选节点返回给 signal-gw。
// Phase 1: 单节点可直接写死返回，但接口留好。
package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"math"
	"net/http"
	"os"
	"os/signal"
	"sync"
	"syscall"
	"time"
)

type SFUNode struct {
	NodeID     string  `json:"nodeId"`
	Host       string  `json:"host"`
	Region     string  `json:"region"`
	CPUPct     float64 `json:"cpuPct"`
	BWOutMbps  float64 `json:"bwOutMbps"`
	BWCapMbps  float64 `json:"bwCapMbps"`
	PeerCount  int     `json:"peerCount"`
	RoomCount  int     `json:"roomCount"`
	LastReport time.Time
}

type Scheduler struct {
	mu    sync.RWMutex
	nodes map[string]*SFUNode
}

func NewScheduler() *Scheduler {
	return &Scheduler{nodes: make(map[string]*SFUNode)}
}

type ReportReq struct {
	NodeID    string  `json:"nodeId"`
	Host      string  `json:"host"`
	Region    string  `json:"region"`
	CPUPct    float64 `json:"cpuPct"`
	BWOutMbps float64 `json:"bwOutMbps"`
	BWCapMbps float64 `json:"bwCapMbps"`
	PeerCount int     `json:"peerCount"`
	RoomCount int     `json:"roomCount"`
}

type DispatchReq struct {
	RoomID         string `json:"roomId"`
	PeerID         string `json:"peerId"`
	Region         string `json:"region"`
	IsPublisher    bool   `json:"isPublisher"`
}

type DispatchResp struct {
	NodeID string `json:"nodeId"`
	Host   string `json:"host"`
}

func (s *Scheduler) handleReport(w http.ResponseWriter, r *http.Request) {
	var req ReportReq
	json.NewDecoder(r.Body).Decode(&req)
	s.mu.Lock()
	defer s.mu.Unlock()
	if n, ok := s.nodes[req.NodeID]; ok {
		n.CPUPct = req.CPUPct
		n.BWOutMbps = req.BWOutMbps
		n.BWCapMbps = req.BWCapMbps
		n.PeerCount = req.PeerCount
		n.RoomCount = req.RoomCount
		n.LastReport = time.Now()
	} else {
		s.nodes[req.NodeID] = &SFUNode{
			NodeID: req.NodeID, Host: req.Host, Region: req.Region,
			CPUPct: req.CPUPct, BWOutMbps: req.BWOutMbps, BWCapMbps: req.BWCapMbps,
			PeerCount: req.PeerCount, RoomCount: req.RoomCount, LastReport: time.Now(),
		}
	}
	w.WriteHeader(200)
}

func (s *Scheduler) handleDispatch(w http.ResponseWriter, r *http.Request) {
	var req DispatchReq
	json.NewDecoder(r.Body).Decode(&req)
	s.mu.RLock()
	defer s.mu.RUnlock()

	// 过滤: 只选 30s 内有心跳且 CPU < 70% 的节点
	candidates := make([]*SFUNode, 0)
	cut := time.Now().Add(-30 * time.Second)
	for _, n := range s.nodes {
		if n.LastReport.After(cut) && n.CPUPct < 70 {
			candidates = append(candidates, n)
		}
	}
	if len(candidates) == 0 {
		// fallback: 返回最新一个（避免没有节点时 join 失败）
		for _, n := range s.nodes {
			candidates = append(candidates, n)
			break
		}
	}
	if len(candidates) == 0 {
		http.Error(w, "no SFU node available", http.StatusServiceUnavailable)
		return
	}

	// 评分: score = 0.35*(1-cpu) + 0.35*(1-bw) + 0.2*(roomAffinity) + 0.1*(regionMatch)
	best := candidates[0]
	bestScore := -1.0
	for _, n := range candidates {
		score := 0.35*(1.0-n.CPUPct/100.0) + 0.35*(1.0-math.Min(n.BWOutMbps/n.BWCapMbps, 1.0))
		if n.Region == req.Region { score += 0.1 }
		if score > bestScore {
			bestScore = score
			best = n
		}
	}
	json.NewEncoder(w).Encode(DispatchResp{NodeID: best.NodeID, Host: best.Host})
}

func main() {
	port := flag.Int("port", 8082, "HTTP listen port")
	flag.Parse()
	s := NewScheduler()
	mux := http.NewServeMux()
	mux.HandleFunc("/api/v1/sfu/report", s.handleReport)
	mux.HandleFunc("/api/v1/dispatch", s.handleDispatch)
	mux.HandleFunc("/health", func(w http.ResponseWriter, r *http.Request) { fmt.Fprint(w, "ok") })
	addr := fmt.Sprintf(":%d", *port)
	log.Printf("[scheduler] listening on %s", addr)
	go func() {
		if err := http.ListenAndServe(addr, mux); err != nil { log.Fatalf("scheduler: %v", err) }
	}()
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	<-sig
}
