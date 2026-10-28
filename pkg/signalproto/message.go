// Package signalproto 定义信令协议的消息信封、类型常量与序列化。
// 全部服务的消息出入口统一走这里，避免散落在各处重复定义。
package signalproto

import "encoding/json"

// Envelope 是信令消息统一信封。
// 客户端请求带 reqId；服务端响应原样带回 reqId + code + msg。
type Envelope struct {
	Type    string          `json:"type"`
	RoomID  string          `json:"roomId,omitempty"`
	PeerID  string          `json:"peerId,omitempty"`
	ReqID   string          `json:"reqId,omitempty"`
	Code    int             `json:"code,omitempty"`
	Msg     string          `json:"msg,omitempty"`
	Payload json.RawMessage `json:"payload,omitempty"`
}

// 客户端 -> 服务端
const (
	TypeJoin            = "join"
	TypeLeave           = "leave"
	TypeCreateTransport = "createTransport"
	TypeConnectTransport = "connectTransport"
	TypeProduce         = "produce"
	TypeConsume         = "consume"
	TypeResumeConsumer  = "resumeConsumer"
	TypePauseConsumer   = "pauseConsumer"
	TypeCloseProducer   = "closeProducer"
	TypePauseProducer   = "pauseProducer"
	TypeResumeProducer  = "resumeProducer"
	TypeRequestMic      = "requestMic"
	TypeCancelMicReq    = "cancelMicRequest"
	TypeGrantMic        = "grantMic"
	TypeDenyMic         = "denyMic"
	TypeKickMic         = "kickMic"
	TypeChat            = "chat"
	TypePing            = "ping"
	TypeStats           = "stats"
)

// 服务端 -> 客户端（推送）
const (
	TypePeerJoined      = "peerJoined"
	TypePeerLeft        = "peerLeft"
	TypeNewProducer     = "newProducer"
	TypeProducerClosed  = "producerClosed"
	TypeProducerPaused  = "producerPaused"
	TypeProducerResumed = "producerResumed"
	TypeMicRequest      = "micRequest"
	TypeMicGranted      = "micGranted"
	TypeMicDenied       = "micDenied"
	TypeMicRevoked      = "micRevoked"
	TypeRoomUpdate      = "roomUpdate"
	TypePong            = "pong"
	TypeSFUNotice       = "sfuNotice"
	TypeError           = "error"
)

// JoinPayload / JoinResponse 等结构体由各服务按需定义，这里只放通用辅助。
func Encode(typ, roomID, peerID, reqID string, payload interface{}) ([]byte, error) {
	raw, err := json.Marshal(payload)
	if err != nil {
		return nil, err
	}
	return json.Marshal(Envelope{
		Type: typ, RoomID: roomID, PeerID: peerID, ReqID: reqID, Payload: raw,
	})
}

func Decode(data []byte) (*Envelope, error) {
	var e Envelope
	if err := json.Unmarshal(data, &e); err != nil {
		return nil, err
	}
	return &e, nil
}
