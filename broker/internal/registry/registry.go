package registry

import (
	"errors"
	"strings"
	"sync"
	"time"

	"agentd-broker/internal/proto"
	"github.com/gorilla/websocket"
)

type AgentConn struct {
	AgentID    string
	DeploymentID string
	Conn       *websocket.Conn
	Connected  time.Time
	LastSeen   time.Time
	RemoteAddr string
	Meta       map[string]any
	DBConnID   int64
	// Soft limits to avoid unbounded memory growth under client abuse or agent bugs.
	// 0 means "unlimited".
	PendingLimit int
	StreamLimit  int

	writeMu sync.Mutex

	pendingMu sync.Mutex
	pending   map[string]chan proto.RelayResponse
	closed    chan struct{}

	streamsMu sync.Mutex
	streams   map[string]chan any
}

type Registry struct {
	mu     sync.RWMutex
	agents map[string]map[string]*AgentConn
}

func New() *Registry {
	return &Registry{agents: make(map[string]map[string]*AgentConn)}
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

func (a *AgentConn) Done() <-chan struct{} {
	if a == nil || a.closed == nil {
		ch := make(chan struct{})
		close(ch)
		return ch
	}
	return a.closed
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
	select {
	case ch <- resp:
	default:
		// Client gave up / timed out; do not block the agent read loop.
	}
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
	if a.StreamLimit > 0 && len(a.streams) >= a.StreamLimit {
		return nil, errors.New("too many active streams")
	}
	ch := make(chan any, 16)
	a.streams[id] = ch
	return ch, nil
}

func (a *AgentConn) DeliverStream(id string, msg any) bool {
	if a == nil {
		return false
	}
	a.streamsMu.Lock()
	ch := a.streams[id]
	a.streamsMu.Unlock()
	if ch == nil {
		return false
	}
	select {
	case ch <- msg:
		return true
	default:
		// If the client cannot keep up, avoid blocking the agent read loop indefinitely.
		// The handler will treat this as a stream termination.
		go a.CloseStream(id)
		return false
	}
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

func (a *AgentConn) Ping() error {
	if a == nil || a.Conn == nil {
		return errors.New("agent conn unavailable")
	}
	a.writeMu.Lock()
	defer a.writeMu.Unlock()
	return a.Conn.WriteControl(websocket.PingMessage, []byte("ping"), time.Now().Add(5*time.Second))
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
	if a.PendingLimit > 0 && len(a.pending) >= a.PendingLimit {
		return nil, errors.New("too many pending requests")
	}
	ch := make(chan proto.RelayResponse, 1)
	a.pending[id] = ch
	return ch, nil
}

func (a *AgentConn) UnregisterPending(id string) {
	if a == nil {
		return
	}
	id = strings.TrimSpace(id)
	if id == "" {
		return
	}
	a.pendingMu.Lock()
	ch := a.pending[id]
	if ch != nil {
		delete(a.pending, id)
	}
	a.pendingMu.Unlock()
	if ch != nil {
		close(ch)
	}
}

func (r *Registry) Upsert(agent *AgentConn) *AgentConn {
	if r == nil || agent == nil || agent.AgentID == "" {
		return nil
	}
	if agent.DeploymentID == "" {
		agent.DeploymentID = "default"
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	m := r.agents[agent.AgentID]
	if m == nil {
		m = make(map[string]*AgentConn)
		r.agents[agent.AgentID] = m
	}
	prev := m[agent.DeploymentID]
	m[agent.DeploymentID] = agent
	return prev
}

func (r *Registry) Get(agentID, deploymentID string) (*AgentConn, bool) {
	if r == nil {
		return nil, false
	}
	if deploymentID == "" {
		return nil, false
	}
	r.mu.RLock()
	defer r.mu.RUnlock()
	m := r.agents[agentID]
	if m == nil {
		return nil, false
	}
	a := m[deploymentID]
	return a, a != nil
}

func (r *Registry) ListByAgent(agentID string) []*AgentConn {
	if r == nil {
		return nil
	}
	r.mu.RLock()
	defer r.mu.RUnlock()
	m := r.agents[agentID]
	if m == nil {
		return nil
	}
	out := make([]*AgentConn, 0, len(m))
	for _, a := range m {
		out = append(out, a)
	}
	return out
}

func (r *Registry) selectLatest(agentID string) (*AgentConn, bool) {
	m := r.agents[agentID]
	if m == nil {
		return nil, false
	}
	var best *AgentConn
	for _, a := range m {
		if a == nil {
			continue
		}
		if best == nil || a.Connected.After(best.Connected) {
			best = a
		}
	}
	if best == nil {
		return nil, false
	}
	return best, true
}

func (r *Registry) Select(agentID, deploymentID string) (*AgentConn, bool) {
	if r == nil {
		return nil, false
	}
	r.mu.RLock()
	defer r.mu.RUnlock()
	if deploymentID != "" {
		m := r.agents[agentID]
		if m == nil {
			return nil, false
		}
		a := m[deploymentID]
		return a, a != nil
	}
	return r.selectLatest(agentID)
}

func (r *Registry) Delete(agentID, deploymentID string) []*AgentConn {
	if r == nil {
		return nil
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	m := r.agents[agentID]
	if m == nil {
		return nil
	}
	removed := []*AgentConn{}
	if deploymentID == "" {
		for _, a := range m {
			if a != nil {
				removed = append(removed, a)
			}
		}
		delete(r.agents, agentID)
		return removed
	}
	if a, ok := m[deploymentID]; ok {
		removed = append(removed, a)
		delete(m, deploymentID)
	}
	if len(m) == 0 {
		delete(r.agents, agentID)
	}
	return removed
}

func (r *Registry) List() []*AgentConn {
	if r == nil {
		return nil
	}
	r.mu.RLock()
	defer r.mu.RUnlock()
	total := 0
	for _, m := range r.agents {
		total += len(m)
	}
	out := make([]*AgentConn, 0, total)
	for _, m := range r.agents {
		for _, a := range m {
			out = append(out, a)
		}
	}
	return out
}

func (r *Registry) AgentCount() int {
	if r == nil {
		return 0
	}
	r.mu.RLock()
	defer r.mu.RUnlock()
	return len(r.agents)
}

func (r *Registry) ConnectionCount() int {
	if r == nil {
		return 0
	}
	r.mu.RLock()
	defer r.mu.RUnlock()
	total := 0
	for _, m := range r.agents {
		total += len(m)
	}
	return total
}

func (r *Registry) Require(agentID, deploymentID string) (*AgentConn, error) {
	a, ok := r.Select(agentID, deploymentID)
	if !ok {
		return nil, errors.New("agent not connected")
	}
	if a.Conn == nil {
		return nil, errors.New("agent connection missing")
	}
	return a, nil
}
