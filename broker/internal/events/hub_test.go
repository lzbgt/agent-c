package events

import (
	"testing"
	"time"
)

func TestHub_SubscribePublishCancel(t *testing.T) {
	h := New()
	ch, cancel := h.Subscribe("user-1")

	h.PublishTo([]string{"user-1"}, Event{Type: "x", Payload: map[string]any{"k": "v"}})

	select {
	case ev := <-ch:
		if ev.Type != "x" {
			t.Fatalf("unexpected event type: %q", ev.Type)
		}
		if ev.TSUnixMS == 0 {
			t.Fatalf("expected TSUnixMS to be set")
		}
	case <-time.After(500 * time.Millisecond):
		t.Fatalf("timeout waiting for event")
	}

	cancel()
	select {
	case _, ok := <-ch:
		if ok {
			t.Fatalf("expected channel to be closed after cancel")
		}
	case <-time.After(500 * time.Millisecond):
		t.Fatalf("timeout waiting for cancel close")
	}
}
