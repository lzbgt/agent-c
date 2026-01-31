package registry

import (
	"errors"
	"sync"
	"time"

	"agentd-broker/internal/proto"
	"github.com/gorilla/websocket"
)

type AgentConn struct {
	AgentID    string
	Conn       *websocket.Conn
	Connected  time.Time
	LastSeen   time.Time
	RemoteAddr string
	Meta       map[string]any
	DBConnID   int64

	writeMu sync.Mutex

	pendingMu sync.Mutex
	pending   map[string]chan proto.RelayResponse
	closed    chan struct{}

	streamsMu sync.Mutex
	streams   map[string]chan any
}

type Registry struct {
	mu     sync.RWMutex
	agents map[string]*AgentConn
}

func New() *Registry {
	return &Registry{agents: make(map[string]*AgentConn)}
}

func (a *AgentConn) InitSession() {
	if a == nil {
		return
	}
	a.pendingMu.Lock()
	defer a.pendingMu.Unlock()
	if a.pending == nil {
		a.pending = make(map[string]chan proto.RelayResponse)
	}
	if a.closed == nil {
		a.closed = make(chan struct{})
	}
	if a.streams == nil {
		a.streams = make(map[string]chan any)
	}
}

func (a *AgentConn) Close() {
	if a == nil {
		return
	}
	a.pendingMu.Lock()
	if a.closed != nil {
		select {
		case <-a.closed:
		default:
			close(a.closed)
		}
	}
	for id, ch := range a.pending {
		_ = id
		close(ch)
	}
	a.pending = make(map[string]chan proto.RelayResponse)
	a.pendingMu.Unlock()

	a.streamsMu.Lock()
	for id, ch := range a.streams {
		_ = id
		close(ch)
	}
	a.streams = make(map[string]chan any)
	a.streamsMu.Unlock()

	if a.Conn != nil {
		_ = a.Conn.Close()
	}
}

func (a *AgentConn) Deliver(resp proto.RelayResponse) {
	if a == nil {
		return
	}
	a.pendingMu.Lock()
	ch := a.pending[resp.ID]
	if ch != nil {
		delete(a.pending, resp.ID)
	}
	a.pendingMu.Unlock()
	if ch == nil {
		return
	}
	ch <- resp
	close(ch)
}

func (a *AgentConn) RegisterStream(id string) (chan any, error) {
	if a == nil {
		return nil, errors.New("nil agent")
	}
	a.streamsMu.Lock()
	defer a.streamsMu.Unlock()
	if a.streams == nil {
		a.streams = make(map[string]chan any)
	}
	if _, ok := a.streams[id]; ok {
		return nil, errors.New("duplicate stream id")
	}
	ch := make(chan any, 16)
	a.streams[id] = ch
	return ch, nil
}

func (a *AgentConn) DeliverStream(id string, msg any) {
	if a == nil {
		return
	}
	a.streamsMu.Lock()
	ch := a.streams[id]
	a.streamsMu.Unlock()
	if ch == nil {
		return
	}
	ch <- msg
}

func (a *AgentConn) CloseStream(id string) {
	if a == nil {
		return
	}
	a.streamsMu.Lock()
	ch := a.streams[id]
	if ch != nil {
		delete(a.streams, id)
	}
	a.streamsMu.Unlock()
	if ch != nil {
		close(ch)
	}
}

func (a *AgentConn) Send(req proto.RelayRequest) error {
	if a == nil || a.Conn == nil {
		return errors.New("agent conn unavailable")
	}
	a.writeMu.Lock()
	defer a.writeMu.Unlock()
	return a.Conn.WriteJSON(req)
}

func (a *AgentConn) SendStream(req proto.StreamRequest) error {
	if a == nil || a.Conn == nil {
		return errors.New("agent conn unavailable")
	}
	a.writeMu.Lock()
	defer a.writeMu.Unlock()
	return a.Conn.WriteJSON(req)
}

func (a *AgentConn) SendAny(v any) error {
	if a == nil || a.Conn == nil {
		return errors.New("agent conn unavailable")
	}
	a.writeMu.Lock()
	defer a.writeMu.Unlock()
	return a.Conn.WriteJSON(v)
}

func (a *AgentConn) RegisterPending(id string) (chan proto.RelayResponse, error) {
	if a == nil {
		return nil, errors.New("nil agent")
	}
	a.pendingMu.Lock()
	defer a.pendingMu.Unlock()
	if a.pending == nil {
		a.pending = make(map[string]chan proto.RelayResponse)
	}
	if _, ok := a.pending[id]; ok {
		return nil, errors.New("duplicate request id")
	}
	ch := make(chan proto.RelayResponse, 1)
	a.pending[id] = ch
	return ch, nil
}

func (r *Registry) Upsert(agent *AgentConn) {
	if r == nil || agent == nil || agent.AgentID == "" {
		return
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	r.agents[agent.AgentID] = agent
}

func (r *Registry) Get(agentID string) (*AgentConn, bool) {
	if r == nil {
		return nil, false
	}
	r.mu.RLock()
	defer r.mu.RUnlock()
	a := r.agents[agentID]
	return a, a != nil
}

func (r *Registry) Delete(agentID string) {
	if r == nil {
		return
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	delete(r.agents, agentID)
}

func (r *Registry) List() []*AgentConn {
	if r == nil {
		return nil
	}
	r.mu.RLock()
	defer r.mu.RUnlock()
	out := make([]*AgentConn, 0, len(r.agents))
	for _, a := range r.agents {
		out = append(out, a)
	}
	return out
}

func (r *Registry) Require(agentID string) (*AgentConn, error) {
	a, ok := r.Get(agentID)
	if !ok {
		return nil, errors.New("agent not connected")
	}
	if a.Conn == nil {
		return nil, errors.New("agent connection missing")
	}
	return a, nil
}
