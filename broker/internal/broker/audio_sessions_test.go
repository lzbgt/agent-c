package broker

import (
	"encoding/json"
	"testing"
	"time"
)

func TestAudioSessionStoreCreateGet(t *testing.T) {
	store := newAudioSessionStore(50 * time.Millisecond)
	sess := store.create("a-1", "default", "user-1", "webrtc")
	if sess == nil {
		t.Fatalf("expected session")
	}
	if sess.agentID != "a-1" {
		t.Fatalf("unexpected agent id")
	}
	got, ok := store.get(sess.id)
	if !ok || got == nil {
		t.Fatalf("expected session")
	}
	// Force expiry.
	got.expiresAt = time.Now().Add(-time.Second)
	if _, ok := store.get(sess.id); ok {
		t.Fatalf("expected expired session")
	}
}

func TestAudioSessionBroadcast(t *testing.T) {
	store := newAudioSessionStore(time.Minute)
	sess := store.create("a-1", "default", "user-1", "webrtc")
	if sess == nil {
		t.Fatalf("expected session")
	}
	_, ch := sess.subscribe()
	payload, _ := json.Marshal(map[string]any{"sdp": "dummy"})
	want := audioSignalEvent{Type: "offer", Payload: payload, From: "webui", TsUnixMs: 1}
	sess.broadcast(want)
	select {
	case got := <-ch:
		if got.Type != want.Type {
			t.Fatalf("unexpected type")
		}
		if string(got.Payload) != string(want.Payload) {
			t.Fatalf("unexpected payload")
		}
	default:
		t.Fatalf("expected signal")
	}
}
