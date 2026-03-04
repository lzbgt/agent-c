package connectors

import "testing"

func TestUpdateStatus(t *testing.T) {
	reg := New()
	if err := reg.Register(Connector{ID: "slack", Status: "ready"}); err != nil {
		t.Fatalf("register: %v", err)
	}
	updated, ok := reg.UpdateStatus("slack", "degraded", "timeout", 42)
	if !ok {
		t.Fatalf("expected update ok")
	}
	if updated.Status != "degraded" {
		t.Fatalf("unexpected status: %q", updated.Status)
	}
	if updated.LastError != "timeout" {
		t.Fatalf("unexpected last_error: %q", updated.LastError)
	}
	if updated.LastSeenUnixMs != 42 {
		t.Fatalf("unexpected ts: %d", updated.LastSeenUnixMs)
	}
}
