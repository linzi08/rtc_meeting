package mediasoupctl

import (
	"encoding/json"
	"fmt"
	"sync"
)

// Router 封装 mediasoup Router（每房间一个）。
type Router struct {
	mu        sync.Mutex
	worker    *Worker
	ID        string
	rtpCaps   json.RawMessage
	closed    bool
	transports map[string]*WebRtcTransport
}

// CreateRouter 在 worker 上创建 Router。
func CreateRouter(w *Worker, mediaCodecs []MediaCodec) (*Router, error) {
	data, err := w.Request("worker.createRouter", map[string]interface{}{
		"mediaCodecs": mediaCodecs,
	})
	if err != nil {
		return nil, fmt.Errorf("createRouter: %w", err)
	}
	var resp struct {
		ID            string          `json:"id"`
		RtpCapabilities json.RawMessage `json:"rtpCapabilities"`
	}
	if err := json.Unmarshal(data, &resp); err != nil {
		return nil, err
	}
	return &Router{
		worker:     w,
		ID:         resp.ID,
		rtpCaps:    resp.RtpCapabilities,
		transports: make(map[string]*WebRtcTransport),
	}, nil
}

// RtpCapabilities 返回 Router 的 RTP 能力（给客户端 device.load 用）。
func (r *Router) RtpCapabilities() json.RawMessage { return r.rtpCaps }

// CreateWebRtcTransport 创建客户端发布/订阅 Transport。
func (r *Router) CreateWebRtcTransport(opts WebRtcTransportOptions) (*WebRtcTransport, error) {
	data, err := r.worker.Request("router.createWebRtcTransport", map[string]interface{}{
		"routerId": r.ID,
		"options":  opts,
	})
	if err != nil {
		return nil, fmt.Errorf("createWebRtcTransport: %w", err)
	}
	var resp struct {
		ID             string          `json:"id"`
		IceParameters  json.RawMessage `json:"iceParameters"`
		IceCandidates  json.RawMessage `json:"iceCandidates"`
		DtlsParameters json.RawMessage `json:"dtlsParameters"`
		SctpParameters json.RawMessage `json:"sctpParameters"`
	}
	if err := json.Unmarshal(data, &resp); err != nil {
		return nil, err
	}
	t := &WebRtcTransport{
		router:     r,
		ID:         resp.ID,
		iceParams:  resp.IceParameters,
		iceCands:   resp.IceCandidates,
		dtlsParams: resp.DtlsParameters,
		sctpParams: resp.SctpParameters,
		producers:  make(map[string]*Producer),
		consumers:  make(map[string]*Consumer),
	}
	r.mu.Lock()
	r.transports[resp.ID] = t
	r.mu.Unlock()
	return t, nil
}

// Close 关闭 Router 及其上所有 Transport。
func (r *Router) Close() {
	r.mu.Lock()
	if r.closed {
		r.mu.Unlock()
		return
	}
	r.closed = true
	r.mu.Unlock()
	r.worker.Request("router.close", map[string]interface{}{"routerId": r.ID})
}

// ---- Transport ----

// MediaCodec 声明 Router 支持的编解码器。
type MediaCodec struct {
	Kind     string  `json:"kind"`
	MimeType string  `json:"mimeType"`
	ClockRate int    `json:"clockRate"`
	Channels int     `json:"channels,omitempty"`
	Parameters map[string]interface{} `json:"parameters,omitempty"`
	RtcpFeedback []RtcpFeedback `json:"rtcpFeedback,omitempty"`
}

type RtcpFeedback struct {
	Type string `json:"type"`
	Parameter string `json:"parameter,omitempty"`
}

type WebRtcTransportOptions struct {
	ListenIps            []ListenInfo `json:"listenIps"`
	EnableUdp            bool         `json:"enableUdp"`
	EnableTcp            bool         `json:"enableTcp"`
	PreferUdp            bool         `json:"preferUdp"`
	InitialAvailableOutgoingBitrate uint32 `json:"initialAvailableOutgoingBitrate,omitempty"`
}

type ListenInfo struct {
	Ip          string `json:"ip"`
	AnnouncedIp string `json:"announcedIp,omitempty"`
}

type WebRtcTransport struct {
	mu         sync.Mutex
	router     *Router
	ID         string
	iceParams  json.RawMessage
	iceCands   json.RawMessage
	dtlsParams json.RawMessage
	sctpParams json.RawMessage
	producers  map[string]*Producer
	consumers  map[string]*Consumer
	closed     bool
}

// Info 返回给客户端的 transport 参数（mediasoup-client 的 connect 参数）。
func (t *WebRtcTransport) Info() map[string]json.RawMessage {
	return map[string]json.RawMessage{
		"id":             json.RawMessage(`"` + t.ID + `"`),
		"iceParameters":  t.iceParams,
		"iceCandidates":  t.iceCands,
		"dtlsParameters": t.dtlsParams,
		"sctpParameters": t.sctpParams,
	}
}

// Connect 完成服务端侧 DTLS 参数回填。
func (t *WebRtcTransport) Connect(dtlsParameters json.RawMessage) error {
	_, err := t.router.worker.Request("transport.connect", map[string]interface{}{
		"transportId":    t.ID,
		"dtlsParameters": dtlsParameters,
	})
	return err
}

// Produce 创建上行 Producer。
func (t *WebRtcTransport) Produce(kind string, rtpParameters json.RawMessage) (*Producer, error) {
	data, err := t.router.worker.Request("transport.produce", map[string]interface{}{
		"transportId":   t.ID,
		"kind":          kind,
		"rtpParameters": rtpParameters,
	})
	if err != nil {
		return nil, err
	}
	var resp struct {
		ID string `json:"id"`
	}
	json.Unmarshal(data, &resp)
	p := &Producer{
		transport: t,
		ID:        resp.ID,
		Kind:      kind,
		rtpParams: rtpParameters,
	}
	t.mu.Lock()
	t.producers[resp.ID] = p
	t.mu.Unlock()
	return p, nil
}

// Consume 创建下行 Consumer。
func (t *WebRtcTransport) Consume(producerID string, rtpCapabilities json.RawMessage) (*Consumer, error) {
	data, err := t.router.worker.Request("transport.consume", map[string]interface{}{
		"transportId":     t.ID,
		"producerId":      producerID,
		"rtpCapabilities": rtpCapabilities,
		"paused":          true, // 默认 paused，客户端 ready 后 resume
	})
	if err != nil {
		return nil, err
	}
	var resp struct {
		ID            string          `json:"id"`
		ProducerID    string          `json:"producerId"`
		Kind          string          `json:"kind"`
		RtpParameters json.RawMessage `json:"rtpParameters"`
	}
	json.Unmarshal(data, &resp)
	c := &Consumer{
		transport:   t,
		ID:          resp.ID,
		producerID:  producerID,
		Kind:        resp.Kind,
		rtpParams:   resp.RtpParameters,
		paused:      true,
	}
	t.mu.Lock()
	t.consumers[resp.ID] = c
	t.mu.Unlock()
	return c, nil
}

// ---- Producer / Consumer ----

type Producer struct {
	mu        sync.Mutex
	transport *WebRtcTransport
	ID        string
	Kind      string
	rtpParams json.RawMessage
	paused    bool
	closed    bool
}

func (p *Producer) Pause() error {
	p.mu.Lock()
	if p.paused || p.closed {
		p.mu.Unlock()
		return nil
	}
	p.paused = true
	p.mu.Unlock()
	_, err := p.transport.router.worker.Request("producer.pause", map[string]interface{}{"producerId": p.ID})
	return err
}

func (p *Producer) Resume() error {
	p.mu.Lock()
	if !p.paused || p.closed {
		p.mu.Unlock()
		return nil
	}
	p.paused = false
	p.mu.Unlock()
	_, err := p.transport.router.worker.Request("producer.resume", map[string]interface{}{"producerId": p.ID})
	return err
}

func (p *Producer) Close() {
	p.mu.Lock()
	if p.closed {
		p.mu.Unlock()
		return
	}
	p.closed = true
	p.mu.Unlock()
	p.transport.router.worker.Request("producer.close", map[string]interface{}{"producerId": p.ID})
}

func (p *Producer) RtpParameters() json.RawMessage { return p.rtpParams }
func (p *Producer) Paused() bool                   { p.mu.Lock(); defer p.mu.Unlock(); return p.paused }

type Consumer struct {
	mu          sync.Mutex
	transport   *WebRtcTransport
	ID          string
	producerID  string
	Kind        string
	rtpParams   json.RawMessage
	paused      bool
	closed      bool
}

func (c *Consumer) Resume() error {
	c.mu.Lock()
	if !c.paused || c.closed {
		c.mu.Unlock()
		return nil
	}
	c.paused = false
	c.mu.Unlock()
	_, err := c.transport.router.worker.Request("consumer.resume", map[string]interface{}{"consumerId": c.ID})
	return err
}

func (c *Consumer) Pause() error {
	c.mu.Lock()
	if c.paused || c.closed {
		c.mu.Unlock()
		return nil
	}
	c.paused = true
	c.mu.Unlock()
	_, err := c.transport.router.worker.Request("consumer.pause", map[string]interface{}{"consumerId": c.ID})
	return err
}

// RequestKeyFrame 向 Producer 侧请求关键帧。
func (c *Consumer) RequestKeyFrame() error {
	_, err := c.transport.router.worker.Request("consumer.requestKeyFrame", map[string]interface{}{"consumerId": c.ID})
	return err
}

func (c *Consumer) SetPreferredLayers(spatialLayer, temporalLayer int) error {
	_, err := c.transport.router.worker.Request("consumer.setPreferredLayers", map[string]interface{}{
		"consumerId": c.ID,
		"spatialLayer": spatialLayer,
		"temporalLayer": temporalLayer,
	})
	return err
}

func (c *Consumer) Close() {
	c.mu.Lock()
	if c.closed {
		c.mu.Unlock()
		return
	}
	c.closed = true
	c.mu.Unlock()
	c.transport.router.worker.Request("consumer.close", map[string]interface{}{"consumerId": c.ID})
}

// Info 返回给客户端的 consumer 参数（mediasoup-client 的 consume 参数）。
func (c *Consumer) Info() map[string]interface{} {
	return map[string]interface{}{
		"id":            c.ID,
		"producerId":    c.producerID,
		"kind":          c.Kind,
		"rtpParameters": c.rtpParams,
		"paused":        c.paused,
	}
}

func (c *Consumer) RtpParameters() json.RawMessage { return c.rtpParams }

// Close 关闭 Transport 及其上所有 Producer/Consumer。
func (t *WebRtcTransport) Close() {
	t.mu.Lock()
	if t.closed {
		t.mu.Unlock()
		return
	}
	t.closed = true
	t.mu.Unlock()
	for _, p := range t.producers { p.Close() }
	for _, c := range t.consumers { c.Close() }
	t.router.worker.Request("transport.close", map[string]interface{}{"transportId": t.ID})
}
