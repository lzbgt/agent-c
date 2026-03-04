package connectors

import (
	"errors"
	"sort"
	"strings"
	"sync"
)

type Connector struct {
	ID             string         `json:"id"`
	Name           string         `json:"name,omitempty"`
	Kind           string         `json:"kind,omitempty"`
	Status         string         `json:"status,omitempty"`
	Description    string         `json:"description,omitempty"`
	LastSeenUnixMs int64          `json:"last_seen_unix_ms,omitempty"`
	LastError      string         `json:"last_error,omitempty"`
	Meta           map[string]any `json:"meta,omitempty"`
}

type Registry struct {
	mu         sync.RWMutex
	connectors map[string]Connector
}

func New() *Registry {
	return &Registry{connectors: make(map[string]Connector)}
}

func (r *Registry) Register(conn Connector) error {
	id := strings.TrimSpace(conn.ID)
	if id == "" {
		return errors.New("connector id required")
	}
	conn.ID = id
	r.mu.Lock()
	r.connectors[id] = conn
	r.mu.Unlock()
	return nil
}

func (r *Registry) UpdateStatus(id, status, lastErr string, tsUnixMs int64) (Connector, bool) {
	trimmed := strings.TrimSpace(id)
	if trimmed == "" {
		return Connector{}, false
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	conn, ok := r.connectors[trimmed]
	if !ok {
		return Connector{}, false
	}
	if strings.TrimSpace(status) != "" {
		conn.Status = strings.TrimSpace(status)
	}
	conn.LastError = strings.TrimSpace(lastErr)
	if tsUnixMs > 0 {
		conn.LastSeenUnixMs = tsUnixMs
	}
	r.connectors[trimmed] = conn
	return conn, true
}

func (r *Registry) List() []Connector {
	r.mu.RLock()
	if len(r.connectors) == 0 {
		r.mu.RUnlock()
		return []Connector{}
	}
	ids := make([]string, 0, len(r.connectors))
	for id := range r.connectors {
		ids = append(ids, id)
	}
	sort.Strings(ids)
	out := make([]Connector, 0, len(ids))
	for _, id := range ids {
		out = append(out, r.connectors[id])
	}
	r.mu.RUnlock()
	return out
}

func (r *Registry) Count() int {
	r.mu.RLock()
	count := len(r.connectors)
	r.mu.RUnlock()
	return count
}
