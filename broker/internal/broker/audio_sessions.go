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
	mode         string
	createdAt    time.Time
	expiresAt    time.Time

	mu             sync.Mutex
	nextSubID      uint64
	subs           map[uint64]chan audioSignalEvent
	signalCount    int64
	lastSignalType string
	lastSignalFrom string
	lastSignalAt   time.Time
}

type audioSessionInfo struct {
	SessionID        string `json:"session_id"`
	AgentID          string `json:"agent_id"`
	DeploymentID     string `json:"deployment_id,omitempty"`
	OwnerSub         string `json:"owner_sub,omitempty"`
	Mode             string `json:"mode,omitempty"`
	CreatedUnixMs    int64  `json:"created_unix_ms"`
	ExpiresUnixMs    int64  `json:"expires_unix_ms"`
	SubscriberCount  int    `json:"subscriber_count"`
	SignalCount      int64  `json:"signal_count"`
	LastSignalType   string `json:"last_signal_type,omitempty"`
	LastSignalFrom   string `json:"last_signal_from,omitempty"`
	LastSignalUnixMs int64  `json:"last_signal_unix_ms,omitempty"`
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
	s.signalCount++
	s.lastSignalType = ev.Type
	s.lastSignalFrom = ev.From
	if ev.TsUnixMs > 0 {
		s.lastSignalAt = time.UnixMilli(ev.TsUnixMs)
	}
	for _, ch := range s.subs {
		select {
		case ch <- ev:
		default:
			// Drop if subscriber is slow.
		}
	}
}

func (s *audioSession) snapshot() audioSessionInfo {
	s.mu.Lock()
	defer s.mu.Unlock()
	info := audioSessionInfo{
		SessionID:       s.id,
		AgentID:         s.agentID,
		DeploymentID:    s.deploymentID,
		OwnerSub:        s.ownerSub,
		Mode:            s.mode,
		CreatedUnixMs:   s.createdAt.UnixMilli(),
		ExpiresUnixMs:   s.expiresAt.UnixMilli(),
		SubscriberCount: len(s.subs),
		SignalCount:     s.signalCount,
		LastSignalType:  s.lastSignalType,
		LastSignalFrom:  s.lastSignalFrom,
	}
	if !s.lastSignalAt.IsZero() {
		info.LastSignalUnixMs = s.lastSignalAt.UnixMilli()
	}
	return info
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

func (s *audioSessionStore) create(agentID, deploymentID, ownerSub, mode string) *audioSession {
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
		mode:         mode,
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

func (s *audioSessionStore) list(agentID, deploymentID string) []audioSessionInfo {
	if s == nil {
		return nil
	}
	now := time.Now()
	s.mu.Lock()
	expired := make([]*audioSession, 0)
	out := make([]*audioSession, 0, len(s.sessions))
	for id, sess := range s.sessions {
		if sess == nil {
			delete(s.sessions, id)
			continue
		}
		if sess.expired(now) {
			delete(s.sessions, id)
			expired = append(expired, sess)
			continue
		}
		if agentID != "" && sess.agentID != agentID {
			continue
		}
		if deploymentID != "" && sess.deploymentID != deploymentID {
			continue
		}
		out = append(out, sess)
	}
	s.mu.Unlock()
	for _, sess := range expired {
		sess.closeAll()
	}
	infos := make([]audioSessionInfo, 0, len(out))
	for _, sess := range out {
		infos = append(infos, sess.snapshot())
	}
	return infos
}
