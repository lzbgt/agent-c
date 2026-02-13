package broker

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"net/http"
	"strings"
)

const maxIdempotencyKeyLen = 128

func idempotencyKeyFromRequest(r *http.Request) (string, error) {
	if r == nil {
		return "", nil
	}
	key := strings.TrimSpace(r.Header.Get("Idempotency-Key"))
	if key == "" {
		key = strings.TrimSpace(r.Header.Get("X-Idempotency-Key"))
	}
	if key == "" {
		return "", nil
	}
	if !isSafeIdempotencyKey(key) {
		return "", errors.New("invalid idempotency key")
	}
	return key, nil
}

func isSafeIdempotencyKey(s string) bool {
	s = strings.TrimSpace(s)
	if s == "" || len(s) > maxIdempotencyKeyLen {
		return false
	}
	for _, r := range s {
		ok := (r >= 'a' && r <= 'z') ||
			(r >= 'A' && r <= 'Z') ||
			(r >= '0' && r <= '9') ||
			r == '-' || r == '_' || r == '.' || r == ':' || r == '@'
		if !ok {
			return false
		}
	}
	return true
}

func idempotencyRequestHash(method, path, query, agentID string, body []byte) string {
	h := sha256.New()
	m := strings.ToUpper(strings.TrimSpace(method))
	h.Write([]byte(m))
	h.Write([]byte{0})
	h.Write([]byte(strings.TrimSpace(path)))
	h.Write([]byte{0})
	h.Write([]byte(strings.TrimSpace(query)))
	h.Write([]byte{0})
	h.Write([]byte(strings.TrimSpace(agentID)))
	h.Write([]byte{0})
	if len(body) > 0 {
		h.Write(body)
	}
	return hex.EncodeToString(h.Sum(nil))
}
