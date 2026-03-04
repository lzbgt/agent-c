package connectors

import (
	"errors"
	"sort"
	"strings"
	"sync"
)

type Connector struct {
	ID          string         `json:"id"`
	Name        string         `json:"name,omitempty"`
	Kind        string         `json:"kind,omitempty"`
	Status      string         `json:"status,omitempty"`
	Description string         `json:"description,omitempty"`
	Meta        map[string]any `json:"meta,omitempty"`
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
