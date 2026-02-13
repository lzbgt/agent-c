package broker

import (
	"net/http/httptest"
	"strings"
	"testing"
)

func TestIdempotencyKeyFromRequest(t *testing.T) {
	req := httptest.NewRequest("POST", "http://broker/v1/orchestrate", nil)
	req.Header.Set("Idempotency-Key", "abc-123")
	key, err := idempotencyKeyFromRequest(req)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if key != "abc-123" {
		t.Fatalf("unexpected key: %q", key)
	}

	req2 := httptest.NewRequest("POST", "http://broker/v1/orchestrate", nil)
	req2.Header.Set("X-Idempotency-Key", "abc-xyz")
	key2, err := idempotencyKeyFromRequest(req2)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if key2 != "abc-xyz" {
		t.Fatalf("unexpected key: %q", key2)
	}

	bad := httptest.NewRequest("POST", "http://broker/v1/orchestrate", nil)
	bad.Header.Set("Idempotency-Key", "bad key")
	if _, err := idempotencyKeyFromRequest(bad); err == nil {
		t.Fatalf("expected error for invalid key")
	}
}

func TestIdempotencyRequestHashIncludesAgent(t *testing.T) {
	body := []byte("payload")
	h1 := idempotencyRequestHash("POST", "/api/v1/run", "", "agent1", "", body)
	h2 := idempotencyRequestHash("POST", "/api/v1/run", "", "agent2", "", body)
	if h1 == h2 {
		t.Fatalf("expected different hashes for different agent ids")
	}
	h3 := idempotencyRequestHash("post", " /api/v1/run ", "", "agent1", "", body)
	h4 := idempotencyRequestHash("POST", "/api/v1/run", "", "agent1", "blue", body)
	if h1 == h4 {
		t.Fatalf("expected different hashes for different deployment ids")
	}
	if strings.TrimSpace(h1) == "" || strings.TrimSpace(h2) == "" || strings.TrimSpace(h3) == "" {
		t.Fatalf("hash should not be empty")
	}
}
