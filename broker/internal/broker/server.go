package broker

import (
	"context"
	"crypto/rand"
	"crypto/tls"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"regexp"
	"strings"
	"time"

	"agentd-broker/internal/auth"
	"agentd-broker/internal/db"
	"agentd-broker/internal/events"
	"agentd-broker/internal/oidc"
	"agentd-broker/internal/proto"
	"agentd-broker/internal/registry"

	"github.com/gorilla/websocket"
)

type Config struct {
	OIDC     *oidc.Verifier
	DB       *db.DB
	Registry *registry.Registry
	Events   *events.Hub

	AgentCNPfx         string
	RequireAgentMTLS   bool
	MaxRequestBodySize int64

	AdminSubs map[string]bool
}

type Server struct {
	cfg Config
	upg websocket.Upgrader
}

func New(cfg Config) (*Server, error) {
	if cfg.DB == nil || cfg.DB.Pool == nil {
		return nil, errors.New("broker db required")
	}
	if cfg.OIDC == nil {
		return nil, errors.New("broker oidc verifier required")
	}
	if cfg.Registry == nil {
		cfg.Registry = registry.New()
	}
	if cfg.Events == nil {
		cfg.Events = events.New()
	}
	if cfg.MaxRequestBodySize == 0 {
		cfg.MaxRequestBodySize = 64 * 1024 * 1024
	}
	s := &Server{cfg: cfg}
	s.upg = websocket.Upgrader{
		ReadBufferSize:  64 * 1024,
		WriteBufferSize: 64 * 1024,
		CheckOrigin: func(r *http.Request) bool {
			_ = r
			return true
		},
	}
	return s, nil
}

func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/agent/connect", s.handleAgentConnect)
	mux.HandleFunc("/v1/agents", s.handleAgents)
	mux.HandleFunc("/v1/events", s.handleEventsSSE)
	// Catch-all for /v1/agents/{id}/... paths.
	mux.HandleFunc("/v1/agents/", s.handleAgentsSubroutes)
	return mux
}

type Principal struct {
	Sub   string
	Admin bool
}

func (s *Server) handleAgents(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case "GET":
		s.handleAgentsList(w, r)
	case "POST":
		s.handleAgentsCreate(w, r)
	default:
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
	}
}

func (s *Server) handleAgentsList(w http.ResponseWriter, r *http.Request) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		http.Error(w, err.Error(), http.StatusUnauthorized)
		return
	}
	type AgentInfo struct {
		AgentID     string         `json:"agent_id"`
		OwnerSub    string         `json:"owner_sub"`
		Enabled     bool           `json:"enabled"`
		CreatedAt   int64          `json:"created_unix_ms"`
		Labels      map[string]any `json:"labels,omitempty"`
		Meta        map[string]any `json:"meta,omitempty"`
		Connected   bool           `json:"connected"`
		ConnectedAt int64          `json:"connected_unix_ms,omitempty"`
		LastSeen    int64          `json:"last_seen_unix_ms,omitempty"`
		RemoteAddr  string         `json:"remote_addr,omitempty"`
	}
	dbAgents, err := s.cfg.DB.ListAgentsForUser(r.Context(), p.Sub)
	if err != nil {
		http.Error(w, "db error", http.StatusInternalServerError)
		return
	}
	out := make([]AgentInfo, 0, len(dbAgents))
	for _, a := range dbAgents {
		info := AgentInfo{
			AgentID:   a.AgentID,
			OwnerSub:  a.OwnerSub,
			Enabled:   a.Enabled,
			CreatedAt: a.CreatedAt.UnixMilli(),
			Labels:    a.Labels(),
			Meta:      a.Meta(),
		}
		if ac, ok := s.cfg.Registry.Get(a.AgentID); ok && ac != nil {
			info.Connected = true
			info.ConnectedAt = ac.Connected.UnixMilli()
			info.LastSeen = ac.LastSeen.UnixMilli()
			info.RemoteAddr = ac.RemoteAddr
		}
		out = append(out, info)
	}
	writeJSON(w, map[string]any{"ok": true, "agents": out})
}

var agentIDRe = regexp.MustCompile(`^[A-Za-z0-9_-]{1,64}$`)

func (s *Server) handleAgentsCreate(w http.ResponseWriter, r *http.Request) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		http.Error(w, err.Error(), http.StatusUnauthorized)
		return
	}
	body, err := readBodyBounded(r.Body, 1024*1024)
	if err != nil {
		http.Error(w, "invalid body", http.StatusBadRequest)
		return
	}
	req := struct {
		AgentID string         `json:"agent_id"`
		Labels  map[string]any `json:"labels,omitempty"`
		Meta    map[string]any `json:"meta,omitempty"`
	}{}
	if len(body) > 0 {
		if err := json.Unmarshal(body, &req); err != nil {
			http.Error(w, "invalid json", http.StatusBadRequest)
			return
		}
	}
	agentID := strings.TrimSpace(req.AgentID)
	if agentID == "" {
		agentID = "a-" + newID()[:12]
	}
	if !agentIDRe.MatchString(agentID) {
		http.Error(w, "invalid agent_id", http.StatusBadRequest)
		return
	}
	if p.Admin && strings.TrimSpace(req.AgentID) != "" && strings.HasPrefix(agentID, "a-") {
		// no-op: allow admin to set ids freely
	}

	a, err := s.cfg.DB.CreateAgent(r.Context(), p.Sub, agentID, req.Labels, req.Meta)
	if err != nil {
		http.Error(w, "create agent failed", http.StatusBadRequest)
		return
	}
	writeJSON(w, map[string]any{
		"ok": true,
		"agent": map[string]any{
			"agent_id":          a.AgentID,
			"owner_sub":         a.OwnerSub,
			"enabled":           a.Enabled,
			"created_unix_ms":   a.CreatedAt.UnixMilli(),
			"labels":            a.Labels(),
			"meta":              a.Meta(),
			"connector_hint_cn": s.cfg.AgentCNPfx + a.AgentID,
		},
	})
}

func (s *Server) handleAgentsSubroutes(w http.ResponseWriter, r *http.Request) {
	// Supported:
	// - /v1/agents/{agent_id}/proxy/<agentd_path>
	// - /v1/agents/{agent_id}/proxy_sse/<agentd_path>
	// - /v1/agents/{agent_id}/disconnect

	rest := strings.TrimPrefix(r.URL.Path, "/v1/agents/")
	parts := strings.SplitN(rest, "/", 3)
	if len(parts) < 2 {
		http.Error(w, "not found", http.StatusNotFound)
		return
	}
	agentID := parts[0]
	action := parts[1]

	if action == "disconnect" {
		s.handleAgentDisconnect(w, r, agentID)
		return
	}
	if action == "delete" {
		s.handleAgentDelete(w, r, agentID)
		return
	}
	if action == "proxy" {
		agentPath := "/"
		if len(parts) == 3 && parts[2] != "" {
			agentPath = "/" + parts[2]
		}
		s.handleAgentProxy(w, r, agentID, agentPath)
		return
	}
	if action == "proxy_sse" {
		agentPath := "/"
		if len(parts) == 3 && parts[2] != "" {
			agentPath = "/" + parts[2]
		}
		s.handleAgentProxySSE(w, r, agentID, agentPath)
		return
	}

	http.Error(w, "not found", http.StatusNotFound)
}

func (s *Server) handleAgentDisconnect(w http.ResponseWriter, r *http.Request, agentID string) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		http.Error(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if !p.Admin {
		http.Error(w, "admin required", http.StatusForbidden)
		return
	}
	if r.Method != "POST" {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	a, ok := s.cfg.Registry.Get(agentID)
	if !ok || a == nil {
		http.Error(w, "agent not connected", http.StatusNotFound)
		return
	}
	a.Close()
	s.cfg.Registry.Delete(agentID)
	writeJSON(w, map[string]any{"ok": true})
}

func (s *Server) handleAgentProxy(w http.ResponseWriter, r *http.Request, agentID, agentPath string) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		http.Error(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if ok, err := s.canAccessAgent(r.Context(), p, agentID); err != nil {
		http.Error(w, "db error", http.StatusInternalServerError)
		return
	} else if !ok {
		http.Error(w, "forbidden", http.StatusForbidden)
		return
	}

	a, err := s.cfg.Registry.Require(agentID)
	if err != nil {
		http.Error(w, "agent not connected", http.StatusBadGateway)
		return
	}

	// Read request body (bounded).
	body, err := readBodyBounded(r.Body, s.cfg.MaxRequestBodySize)
	if err != nil {
		http.Error(w, "request body too large", http.StatusRequestEntityTooLarge)
		return
	}

	// Build agent-facing request.
	headers := map[string]string{}
	for k, vv := range r.Header {
		if len(vv) == 0 {
			continue
		}
		kl := strings.ToLower(k)
		// Never forward broker control-plane auth headers to the agent.
		if kl == "authorization" || kl == "host" || kl == "connection" {
			continue
		}
		// Keep first value only (agentd endpoints don't require multi-value headers).
		headers[k] = vv[0]
	}
	headers["X-Agentd-Broker-User"] = p.Sub

	start := time.Now()
	status := 0
	errStr := ""
	reqID := newID()
	ch, err := a.RegisterPending(reqID)
	if err != nil {
		http.Error(w, "broker internal error", http.StatusInternalServerError)
		return
	}

	msg := proto.RelayRequest{
		Type: proto.TypeHTTPRequest,
		ID:   reqID,
		Req: proto.HTTPRequest{
			Method:  r.Method,
			Path:    agentPath,
			Query:   r.URL.RawQuery,
			Headers: headers,
			BodyB64: base64.StdEncoding.EncodeToString(body),
		},
	}

	if err := a.Send(msg); err != nil {
		s.cfg.Registry.Delete(agentID)
		errStr = "agent send failed"
		_ = s.cfg.DB.InsertRelayAudit(r.Context(), p.Sub, agentID, r.Method, agentPath, status, int(time.Since(start).Milliseconds()), errStr)
		http.Error(w, "agent send failed", http.StatusBadGateway)
		return
	}

	// Wait for response.
	timeout := 60 * time.Second
	if dl, ok := r.Context().Deadline(); ok {
		timeout = time.Until(dl)
	}
	select {
	case resp, ok := <-ch:
		if !ok {
			errStr = "agent disconnected"
			_ = s.cfg.DB.InsertRelayAudit(r.Context(), p.Sub, agentID, r.Method, agentPath, status, int(time.Since(start).Milliseconds()), errStr)
			http.Error(w, "agent disconnected", http.StatusBadGateway)
			return
		}
		if resp.Err != "" {
			errStr = resp.Err
			_ = s.cfg.DB.InsertRelayAudit(r.Context(), p.Sub, agentID, r.Method, agentPath, status, int(time.Since(start).Milliseconds()), errStr)
			http.Error(w, resp.Err, http.StatusBadGateway)
			return
		}
		// Apply response headers (sanitized).
		for k, v := range resp.Resp.Headers {
			kl := strings.ToLower(k)
			if kl == "content-length" || kl == "connection" || kl == "transfer-encoding" {
				continue
			}
			w.Header().Set(k, v)
		}
		status = resp.Resp.Status
		w.WriteHeader(status)
		b, err := base64.StdEncoding.DecodeString(resp.Resp.BodyB64)
		if err != nil {
			errStr = "invalid agent body"
			_ = s.cfg.DB.InsertRelayAudit(r.Context(), p.Sub, agentID, r.Method, agentPath, status, int(time.Since(start).Milliseconds()), errStr)
			http.Error(w, "invalid agent body", http.StatusBadGateway)
			return
		}
		_, _ = w.Write(b)
		_ = s.cfg.DB.InsertRelayAudit(r.Context(), p.Sub, agentID, r.Method, agentPath, status, int(time.Since(start).Milliseconds()), errStr)
		s.cfg.Events.PublishTo([]string{p.Sub}, events.Event{
			Type:    "relay_audit",
			AgentID: agentID,
			UserSub: p.Sub,
			Payload: map[string]any{
				"method":     r.Method,
				"path":       agentPath,
				"status":     status,
				"latency_ms": int(time.Since(start).Milliseconds()),
				"error":      errStr,
			},
		})
	case <-time.After(timeout):
		errStr = "agent timeout"
		_ = s.cfg.DB.InsertRelayAudit(r.Context(), p.Sub, agentID, r.Method, agentPath, status, int(time.Since(start).Milliseconds()), errStr)
		http.Error(w, "agent timeout", http.StatusGatewayTimeout)
		return
	}
}

func (s *Server) handleAgentProxySSE(w http.ResponseWriter, r *http.Request, agentID, agentPath string) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		http.Error(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if ok, err := s.canAccessAgent(r.Context(), p, agentID); err != nil {
		http.Error(w, "db error", http.StatusInternalServerError)
		return
	} else if !ok {
		http.Error(w, "forbidden", http.StatusForbidden)
		return
	}
	a, err := s.cfg.Registry.Require(agentID)
	if err != nil {
		http.Error(w, "agent not connected", http.StatusBadGateway)
		return
	}
	fl, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "streaming unsupported", http.StatusInternalServerError)
		return
	}

	body, err := readBodyBounded(r.Body, s.cfg.MaxRequestBodySize)
	if err != nil {
		http.Error(w, "request body too large", http.StatusRequestEntityTooLarge)
		return
	}

	headers := map[string]string{}
	for k, vv := range r.Header {
		if len(vv) == 0 {
			continue
		}
		kl := strings.ToLower(k)
		if kl == "authorization" || kl == "host" || kl == "connection" {
			continue
		}
		headers[k] = vv[0]
	}
	headers["X-Agentd-Broker-User"] = p.Sub

	streamID := newID()
	streamCh, err := a.RegisterStream(streamID)
	if err != nil {
		http.Error(w, "broker internal error", http.StatusInternalServerError)
		return
	}
	defer a.CloseStream(streamID)

	sendCancel := func() {
		_ = a.SendAny(proto.StreamCancel{
			Type: proto.TypeHTTPStreamCancel,
			ID:   streamID,
		})
	}

	msg := proto.StreamRequest{
		Type: proto.TypeHTTPStreamRequest,
		ID:   streamID,
		Req: proto.HTTPRequest{
			Method:  r.Method,
			Path:    agentPath,
			Query:   r.URL.RawQuery,
			Headers: headers,
			BodyB64: base64.StdEncoding.EncodeToString(body),
		},
	}
	if err := a.SendStream(msg); err != nil {
		s.cfg.Registry.Delete(agentID)
		http.Error(w, "agent send failed", http.StatusBadGateway)
		return
	}

	// Wait for stream start (status + headers) before writing client response.
	timeout := 15 * time.Second
	if dl, ok := r.Context().Deadline(); ok {
		timeout = time.Until(dl)
	}
	startTimer := time.NewTimer(timeout)
	defer startTimer.Stop()

	var started bool
	for !started {
		select {
		case <-r.Context().Done():
			sendCancel()
			return
		case <-startTimer.C:
			sendCancel()
			http.Error(w, "agent stream start timeout", http.StatusGatewayTimeout)
			return
		case m, ok := <-streamCh:
			if !ok {
				http.Error(w, "agent disconnected", http.StatusBadGateway)
				return
			}
			ss, ok := m.(proto.StreamStart)
			if !ok {
				if sp, ok := m.(*proto.StreamStart); ok && sp != nil {
					ss = *sp
					ok = true
				}
			}
			if !ok {
				// Ignore until we get StreamStart.
				continue
			}
			for k, v := range ss.Resp.Headers {
				kl := strings.ToLower(k)
				if kl == "content-length" || kl == "connection" || kl == "transfer-encoding" {
					continue
				}
				w.Header().Set(k, v)
			}
			if strings.TrimSpace(w.Header().Get("Content-Type")) == "" {
				w.Header().Set("Content-Type", "text/event-stream")
			}
			w.Header().Set("Cache-Control", "no-cache")
			w.Header().Set("Connection", "keep-alive")
			w.WriteHeader(ss.Resp.Status)
			fl.Flush()
			started = true
		}
	}

	// Forward chunks until end, disconnect, or client cancel.
	for {
		select {
		case <-r.Context().Done():
			sendCancel()
			return
		case m, ok := <-streamCh:
			if !ok {
				return
			}
			switch v := m.(type) {
			case proto.StreamChunk:
				b, err := base64.StdEncoding.DecodeString(v.Data)
				if err != nil {
					sendCancel()
					return
				}
				if len(b) > 0 {
					if _, err := w.Write(b); err != nil {
						sendCancel()
						return
					}
					fl.Flush()
				}
			case *proto.StreamChunk:
				if v == nil {
					continue
				}
				b, err := base64.StdEncoding.DecodeString(v.Data)
				if err != nil {
					sendCancel()
					return
				}
				if len(b) > 0 {
					if _, err := w.Write(b); err != nil {
						sendCancel()
						return
					}
					fl.Flush()
				}
			case proto.StreamEnd, *proto.StreamEnd:
				return
			default:
				// ignore
			}
		}
	}
}

func (s *Server) handleAgentConnect(w http.ResponseWriter, r *http.Request) {
	if r.Method != "GET" {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	cert, certErr := auth.VerifiedClientLeaf(r)
	if certErr != nil {
		if s.cfg.RequireAgentMTLS {
			http.Error(w, "agent mTLS required", http.StatusUnauthorized)
			return
		}
	}
	certAgentID := ""
	if cert != nil {
		id, err := auth.AgentIDFromCertCN(cert, s.cfg.AgentCNPfx)
		if err != nil {
			http.Error(w, err.Error(), http.StatusUnauthorized)
			return
		}
		certAgentID = id
	}

	conn, err := s.upg.Upgrade(w, r, nil)
	if err != nil {
		return
	}
	conn.SetReadLimit(64 * 1024 * 1024)

	// Handshake: require hello message.
	var hello proto.Hello
	if err := conn.ReadJSON(&hello); err != nil {
		_ = conn.Close()
		return
	}
	if hello.Type != proto.TypeHello {
		_ = conn.Close()
		return
	}
	agentID := strings.TrimSpace(hello.AgentID)
	if agentID == "" {
		agentID = certAgentID
	}
	if agentID == "" {
		_ = conn.Close()
		return
	}
	if certAgentID != "" && agentID != certAgentID {
		_ = conn.WriteJSON(proto.HelloAck{Type: "hello_ack", OK: false, Error: "agent_id mismatch vs client cert"})
		_ = conn.Close()
		return
	}

	enabled, err := s.cfg.DB.AgentEnabled(r.Context(), agentID)
	if err != nil {
		_ = conn.WriteJSON(proto.HelloAck{Type: proto.TypeHelloAck, OK: false, Error: "agent not registered"})
		_ = conn.Close()
		return
	}
	if !enabled {
		_ = conn.WriteJSON(proto.HelloAck{Type: proto.TypeHelloAck, OK: false, Error: "agent disabled"})
		_ = conn.Close()
		return
	}

	connID, err := s.cfg.DB.InsertConnection(r.Context(), agentID, r.RemoteAddr, hello.Meta)
	if err != nil {
		_ = conn.WriteJSON(proto.HelloAck{Type: proto.TypeHelloAck, OK: false, Error: "db connection record failed"})
		_ = conn.Close()
		return
	}

	ac := &registry.AgentConn{
		AgentID:    agentID,
		Conn:       conn,
		Connected:  time.Now(),
		LastSeen:   time.Now(),
		RemoteAddr: r.RemoteAddr,
		Meta:       hello.Meta,
		DBConnID:   connID,
	}
	ac.InitSession()
	s.cfg.Registry.Upsert(ac)

	_ = conn.WriteJSON(proto.HelloAck{Type: proto.TypeHelloAck, OK: true, AgentID: agentID})

	if subs, err := s.cfg.DB.ListAgentMemberSubs(r.Context(), agentID); err == nil {
		s.cfg.Events.PublishTo(subs, events.Event{
			Type:    "agent_connected",
			AgentID: agentID,
			Payload: map[string]any{
				"remote_addr": r.RemoteAddr,
			},
		})
	}

	go s.agentReadLoop(ac)
}

func (s *Server) agentReadLoop(a *registry.AgentConn) {
	defer func() {
		if a != nil {
			_ = s.cfg.DB.MarkConnectionDisconnected(context.Background(), a.DBConnID)
			a.Close()
			s.cfg.Registry.Delete(a.AgentID)
			if subs, err := s.cfg.DB.ListAgentMemberSubs(context.Background(), a.AgentID); err == nil {
				s.cfg.Events.PublishTo(subs, events.Event{
					Type:    "agent_disconnected",
					AgentID: a.AgentID,
				})
			}
		}
	}()
	for {
		_, raw, err := a.Conn.ReadMessage()
		if err != nil {
			return
		}
		a.LastSeen = time.Now()

		var env struct {
			Type string `json:"type"`
		}
		if err := json.Unmarshal(raw, &env); err != nil {
			continue
		}
		switch env.Type {
		case proto.TypeHTTPResp:
			var rr proto.RelayResponse
			if err := json.Unmarshal(raw, &rr); err != nil {
				continue
			}
			a.Deliver(rr)
		case proto.TypeHTTPStreamStart:
			var st proto.StreamStart
			if err := json.Unmarshal(raw, &st); err != nil {
				continue
			}
			a.DeliverStream(st.ID, st)
		case proto.TypeHTTPStreamChunk:
			var ch proto.StreamChunk
			if err := json.Unmarshal(raw, &ch); err != nil {
				continue
			}
			a.DeliverStream(ch.ID, ch)
		case proto.TypeHTTPStreamEnd:
			var en proto.StreamEnd
			if err := json.Unmarshal(raw, &en); err != nil {
				continue
			}
			a.DeliverStream(en.ID, en)
			a.CloseStream(en.ID)
		default:
			// ignore
		}
	}
}

func (s *Server) requirePrincipal(r *http.Request) (*Principal, error) {
	if r == nil {
		return nil, errors.New("nil request")
	}
	pr, err := s.cfg.OIDC.AuthenticateRequest(r.Context(), r)
	if err != nil {
		return nil, err
	}
	p := &Principal{
		Sub:   pr.Sub,
		Admin: s.cfg.AdminSubs != nil && s.cfg.AdminSubs[pr.Sub],
	}
	if err := s.cfg.DB.EnsureUser(r.Context(), p.Sub); err != nil {
		return nil, err
	}
	return p, nil
}

func (s *Server) canAccessAgent(ctx context.Context, p *Principal, agentID string) (bool, error) {
	if p == nil {
		return false, nil
	}
	if p.Admin {
		return true, nil
	}
	return s.cfg.DB.UserCanAccessAgent(ctx, p.Sub, agentID)
}

func (s *Server) handleAgentDelete(w http.ResponseWriter, r *http.Request, agentID string) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		http.Error(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if r.Method != "POST" && r.Method != "DELETE" {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if p.Admin {
		if err := s.cfg.DB.DeleteAgent(r.Context(), agentID); err != nil {
			http.Error(w, "delete failed", http.StatusBadRequest)
			return
		}
		writeJSON(w, map[string]any{"ok": true})
		return
	}
	if err := s.cfg.DB.DeleteAgentIfOwner(r.Context(), p.Sub, agentID); err != nil {
		http.Error(w, "not owner", http.StatusForbidden)
		return
	}
	writeJSON(w, map[string]any{"ok": true})
}

func (s *Server) handleEventsSSE(w http.ResponseWriter, r *http.Request) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		http.Error(w, err.Error(), http.StatusUnauthorized)
		return
	}
	fl, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "streaming unsupported", http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("Connection", "keep-alive")

	ch, cancel := s.cfg.Events.Subscribe(p.Sub)
	defer cancel()

	_, _ = w.Write([]byte(":ok\n\n"))
	fl.Flush()

	for {
		select {
		case <-r.Context().Done():
			return
		case ev, ok := <-ch:
			if !ok {
				return
			}
			b, _ := json.Marshal(ev)
			// Minimal SSE framing. Clients can decode JSON from data.
			_, _ = w.Write([]byte("event: " + ev.Type + "\n"))
			_, _ = w.Write([]byte("data: "))
			_, _ = w.Write(b)
			_, _ = w.Write([]byte("\n\n"))
			fl.Flush()
		}
	}
}

func writeJSON(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	b, _ := json.Marshal(v)
	_, _ = w.Write(b)
}

func readBodyBounded(r io.Reader, limit int64) ([]byte, error) {
	lr := &io.LimitedReader{R: r, N: limit + 1}
	b, err := io.ReadAll(lr)
	if err != nil {
		return nil, err
	}
	if int64(len(b)) > limit {
		return nil, errors.New("body too large")
	}
	return b, nil
}

func newID() string {
	var b [16]byte
	if _, err := rand.Read(b[:]); err == nil {
		return hex.EncodeToString(b[:])
	}
	return fmt.Sprintf("%d", time.Now().UnixNano())
}

func Serve(addr, tlsCert, tlsKey string, tlsCfg *tls.Config, h http.Handler) error {
	srv := &http.Server{
		Addr:              addr,
		Handler:           h,
		ReadHeaderTimeout: 5 * time.Second,
		TLSConfig:         tlsCfg,
	}
	if strings.TrimSpace(tlsCert) != "" && strings.TrimSpace(tlsKey) != "" {
		return srv.ListenAndServeTLS(strings.TrimSpace(tlsCert), strings.TrimSpace(tlsKey))
	}
	return srv.ListenAndServe()
}

func Shutdown(ctx context.Context, srv *http.Server) error {
	if srv == nil {
		return nil
	}
	return srv.Shutdown(ctx)
}
