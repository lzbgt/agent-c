package events

import (
	"crypto/rand"
	"encoding/hex"
	"sync"
	"time"
)

// Event is a broker-originated event intended for SSE consumers.
//
// The hub is intentionally "dumb" and does no authorization; the broker must only
// publish events to subjects that are allowed to see them.
type Event struct {
	Type      string         `json:"type"`
	TSUnixMS  int64          `json:"ts_unix_ms"`
	AgentID   string         `json:"agent_id,omitempty"`
	UserSub   string         `json:"user_sub,omitempty"`
	Payload   map[string]any `json:"payload,omitempty"`
	EventID   string         `json:"event_id,omitempty"`
	TraceID   string         `json:"trace_id,omitempty"`
	ExtraInfo map[string]any `json:"extra,omitempty"`
}

type Recorder interface {
	Record(userSubs []string, e Event) error
}

type Hub struct {
	mu        sync.Mutex
	nextID    int
	byUser    map[string]map[int]chan Event
	closing   bool
	recorder  Recorder
	recordErr func(error)
}

func New() *Hub {
	return &Hub{
		byUser: make(map[string]map[int]chan Event),
	}
}

func (h *Hub) SetRecorder(r Recorder) {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.recorder = r
}

func (h *Hub) SetRecordErrorHandler(fn func(error)) {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.recordErr = fn
}

func (h *Hub) Subscribe(userSub string) (<-chan Event, func()) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.byUser == nil {
		h.byUser = make(map[string]map[int]chan Event)
	}
	h.nextID++
	id := h.nextID
	ch := make(chan Event, 64)
	m := h.byUser[userSub]
	if m == nil {
		m = make(map[int]chan Event)
		h.byUser[userSub] = m
	}
	m[id] = ch
	cancel := func() {
		h.mu.Lock()
		defer h.mu.Unlock()
		m := h.byUser[userSub]
		if m == nil {
			return
		}
		c := m[id]
		if c == nil {
			return
		}
		delete(m, id)
		if len(m) == 0 {
			delete(h.byUser, userSub)
		}
		close(c)
	}
	return ch, cancel
}

func (h *Hub) PublishTo(userSubs []string, e Event) {
	if len(userSubs) == 0 {
		return
	}
	if e.TSUnixMS == 0 {
		e.TSUnixMS = time.Now().UnixMilli()
	}
	if e.EventID == "" {
		e.EventID = newEventID()
	}
	h.mu.Lock()
	recorder, errHandler := h.recorder, h.recordErr
	h.mu.Unlock()
	if recorder != nil {
		if err := recorder.Record(userSubs, e); err != nil && errHandler != nil {
			errHandler(err)
		}
	}
	h.mu.Lock()
	defer h.mu.Unlock()
	for _, sub := range userSubs {
		m := h.byUser[sub]
		if len(m) == 0 {
			continue
		}
		for _, ch := range m {
			if ch == nil {
				continue
			}
			select {
			case ch <- e:
			default:
				// Drop on backpressure; SSE consumers should keep up.
			}
		}
	}
}

func newEventID() string {
	var b [16]byte
	if _, err := rand.Read(b[:]); err != nil {
		return hex.EncodeToString([]byte(time.Now().Format("20060102150405.000000000")))
	}
	return hex.EncodeToString(b[:])
}
