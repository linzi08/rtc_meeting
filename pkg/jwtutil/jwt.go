// Package jwtutil 提供最小化 JWT 签发与校验（HS256）。
// 生产可换 golang-jwt/jwt，这里自实现避免额外依赖。
package jwtutil

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"time"
)

type Claims struct {
	PeerID   string `json:"peerId"`
	RoomID   string `json:"roomId,omitempty"`
	Role     string `json:"role,omitempty"`
	Exp      int64  `json:"exp"`
}

var ErrExpired = errors.New("token expired")

func Sign(secret string, c Claims) string {
	c.Exp = time.Now().Add(24 * time.Hour).Unix()
	header := b64([]byte(`{"alg":"HS256","typ":"JWT"}`))
	body := b64(mustJSON(c))
	sig := hmacSHA256(secret, header+"."+body)
	return header + "." + body + "." + sig
}

func Verify(secret, token string) (*Claims, error) {
	parts := strings.Split(token, ".")
	if len(parts) != 3 {
		return nil, errors.New("malformed token")
	}
	sig := hmacSHA256(secret, parts[0]+"."+parts[1])
	if !hmac.Equal([]byte(sig), []byte(parts[2])) {
		return nil, errors.New("signature mismatch")
	}
	raw, err := base64.RawURLEncoding.DecodeString(parts[1])
	if err != nil {
		return nil, err
	}
	var c Claims
	if err := json.Unmarshal(raw, &c); err != nil {
		return nil, err
	}
	if c.Exp < time.Now().Unix() {
		return nil, ErrExpired
	}
	return &c, nil
}

func b64(b []byte) string { return base64.RawURLEncoding.EncodeToString(b) }

func mustJSON(v interface{}) []byte {
	b, _ := json.Marshal(v)
	return b
}

func hmacSHA256(secret, data string) string {
	m := hmac.New(sha256.New, []byte(secret))
	m.Write([]byte(data))
	return b64(m.Sum(nil))
}

var _ = fmt.Sprintf
