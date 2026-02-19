package broker

import (
	"encoding/json"
	"sync"
	"time"
)

const defaultAudioSessionTTL = 15 * time.Minute
const audioSessionChannelSize = 32

type audioSignalEvent struct {
	Type     string          `json:"type"`
	Payload  json.RawMessage `json:"payload,omitempty"`
	From     string          `json:"from,omitempty"`
	TsUnixMs int64           `json:"ts_unix_ms"`
}

type audioSession struct {
	id           string
	agentID      string
	deploymentID string
	ownerSub     string
	createdAt    time.Time
	expiresAt    time.Time

	mu        sync.Mutex
	nextSubID uint64
	subs      map[uint64]chan audioSignalEvent
}

func (s *audioSession) expired(now time.Time) bool {
	if s == nil || s.expiresAt.IsZero() {
		return false
	}
	return now.After(s.expiresAt)
}

func (s *audioSession) subscribe() (uint64, chan audioSignalEvent) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.subs == nil {
		s.subs = map[uint64]chan audioSignalEvent{}
	}
	s.nextSubID++
	id := s.nextSubID
	ch := make(chan audioSignalEvent, audioSessionChannelSize)
	s.subs[id] = ch
	return id, ch
}

func (s *audioSession) unsubscribe(id uint64) {
	s.mu.Lock()
	defer s.mu.Unlock()
	ch, ok := s.subs[id]
	if !ok {
		return
	}
	delete(s.subs, id)
	close(ch)
}

func (s *audioSession) broadcast(ev audioSignalEvent) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, ch := range s.subs {
		select {
		case ch <- ev:
		default:
			// Drop if subscriber is slow.
		}
	}
}

func (s *audioSession) closeAll() {
	s.mu.Lock()
	defer s.mu.Unlock()
	for id, ch := range s.subs {
		delete(s.subs, id)
		close(ch)
	}
}

type audioSessionStore struct {
	mu       sync.Mutex
	sessions map[string]*audioSession
	ttl      time.Duration
}

func newAudioSessionStore(ttl time.Duration) *audioSessionStore {
	if ttl <= 0 {
		ttl = defaultAudioSessionTTL
	}
	return &audioSessionStore{
		sessions: map[string]*audioSession{},
		ttl:      ttl,
	}
}

func (s *audioSessionStore) create(agentID, deploymentID, ownerSub string) *audioSession {
	if s == nil {
		return nil
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	id := "aud_" + newID()[:12]
	now := time.Now()
	sess := &audioSession{
		id:           id,
		agentID:      agentID,
		deploymentID: deploymentID,
		ownerSub:     ownerSub,
		createdAt:    now,
		expiresAt:    now.Add(s.ttl),
		subs:         map[uint64]chan audioSignalEvent{},
	}
	s.sessions[id] = sess
	return sess
}

func (s *audioSessionStore) get(id string) (*audioSession, bool) {
	if s == nil {
		return nil, false
	}
	now := time.Now()
	s.mu.Lock()
	defer s.mu.Unlock()
	sess := s.sessions[id]
	if sess == nil {
		return nil, false
	}
	if sess.expired(now) {
		delete(s.sessions, id)
		sess.closeAll()
		return nil, false
	}
	return sess, true
}

func (s *audioSessionStore) delete(id string) {
	if s == nil {
		return
	}
	s.mu.Lock()
	sess := s.sessions[id]
	delete(s.sessions, id)
	s.mu.Unlock()
	if sess != nil {
		sess.closeAll()
	}
}
