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

type recordSpy struct {
	userSubs []string
	event    Event
	err      error
}

func (r *recordSpy) Record(userSubs []string, e Event) error {
	r.userSubs = append([]string{}, userSubs...)
	r.event = e
	return r.err
}

func TestHub_RecordAndEventID(t *testing.T) {
	h := New()
	spy := &recordSpy{}
	h.SetRecorder(spy)

	h.PublishTo([]string{"user-1", "user-2"}, Event{Type: "test", Payload: map[string]any{"k": "v"}})

	if len(spy.userSubs) != 2 {
		t.Fatalf("expected recorder to receive subs, got %v", spy.userSubs)
	}
	if spy.event.Type != "test" {
		t.Fatalf("unexpected recorded event type: %q", spy.event.Type)
	}
	if spy.event.EventID == "" {
		t.Fatalf("expected EventID to be set")
	}
	if spy.event.TSUnixMS == 0 {
		t.Fatalf("expected TSUnixMS to be set")
	}
}
