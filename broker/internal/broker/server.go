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
	"sync"
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
	OIDC               *oidc.Verifier
	ClientAuth         *auth.ClientAuth
	ClientAuthFallback bool
	ClientAuthStrict   bool
	DB                 *db.DB
	Registry           *registry.Registry
	Events             *events.Hub

	AgentCNPfx         string
	RequireAgentMTLS   bool
	MaxRequestBodySize int64
	MaxPendingPerAgent int
	MaxStreamsPerAgent int

	// If set, enables CORS for browser clients.
	// Values should be full origins like "https://example.com".
	AllowedOrigins []string

	// SSE keepalive comment interval. 0 uses a safe default.
	SSEKeepaliveInterval time.Duration
	// Readiness check cache interval. 0 uses a safe default.
	ReadinessCacheInterval time.Duration

	AdminSubs map[string]bool
}

type Server struct {
	cfg Config
	upg websocket.Upgrader

	startTime time.Time
	readyMu   sync.Mutex
	readyAt   time.Time
	readyOK   bool
	readyErr  string
	readyBusy bool

	clientAuthMu        sync.RWMutex
	clientAuth          *auth.ClientAuth
	clientAuthStatusMu  sync.RWMutex
	clientAuthLastAt    time.Time
	clientAuthLastOK    bool
	clientAuthLastError string
}

func New(cfg Config) (*Server, error) {
	if cfg.DB == nil || cfg.DB.Pool == nil {
		return nil, errors.New("broker db required")
	}
	if cfg.OIDC == nil && cfg.ClientAuth == nil {
		return nil, errors.New("broker auth required (oidc or client auth)")
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
	if cfg.MaxPendingPerAgent == 0 {
		cfg.MaxPendingPerAgent = 256
	}
	if cfg.MaxStreamsPerAgent == 0 {
		cfg.MaxStreamsPerAgent = 64
	}
	if cfg.SSEKeepaliveInterval == 0 {
		cfg.SSEKeepaliveInterval = 15 * time.Second
	}
	if cfg.ReadinessCacheInterval == 0 {
		cfg.ReadinessCacheInterval = 5 * time.Second
	}
	s := &Server{cfg: cfg, startTime: time.Now()}
	s.clientAuth = cfg.ClientAuth
	if cfg.ClientAuth != nil {
		s.clientAuthLastAt = time.Now()
		s.clientAuthLastOK = true
	}
	s.upg = websocket.Upgrader{
		ReadBufferSize:  64 * 1024,
		WriteBufferSize: 64 * 1024,
		CheckOrigin: func(r *http.Request) bool {
			origin := strings.TrimSpace(r.Header.Get("Origin"))
			if origin == "" {
				return true
			}
			return originAllowed(origin, cfg.AllowedOrigins)
		},
	}
	return s, nil
}

func (s *Server) getClientAuth() *auth.ClientAuth {
	if s == nil {
		return nil
	}
	s.clientAuthMu.RLock()
	ca := s.clientAuth
	s.clientAuthMu.RUnlock()
	return ca
}

func (s *Server) SetClientAuth(ca *auth.ClientAuth) {
	if s == nil {
		return
	}
	s.clientAuthMu.Lock()
	s.clientAuth = ca
	s.clientAuthMu.Unlock()
}

func (s *Server) SetClientAuthStatus(ok bool, errStr string) {
	if s == nil {
		return
	}
	s.clientAuthStatusMu.Lock()
	s.clientAuthLastAt = time.Now()
	s.clientAuthLastOK = ok
	s.clientAuthLastError = errStr
	s.clientAuthStatusMu.Unlock()
}

func (s *Server) getClientAuthStatus() (bool, time.Time, string) {
	if s == nil {
		return false, time.Time{}, "nil server"
	}
	s.clientAuthStatusMu.RLock()
	ok := s.clientAuthLastOK
	at := s.clientAuthLastAt
	errStr := s.clientAuthLastError
	s.clientAuthStatusMu.RUnlock()
	return ok, at, errStr
}

func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/healthz", s.handleHealthz)
	mux.HandleFunc("/readyz", s.handleReadyz)
	mux.HandleFunc("/metrics", s.handleMetrics)
	mux.HandleFunc("/v1/agent/connect", s.handleAgentConnect)
	mux.HandleFunc("/v1/agents", s.handleAgents)
	mux.HandleFunc("/v1/orchestrate", s.handleOrchestrate)
	mux.HandleFunc("/v1/trace", s.handleTrace)
	mux.HandleFunc("/v1/events", s.handleEventsSSE)
	// Catch-all for /v1/agents/{id}/... paths.
	mux.HandleFunc("/v1/agents/", s.handleAgentsSubroutes)
	h := http.Handler(mux)
	h = withAccessLog(h)
	if s.cfg.AllowedOrigins != nil && len(s.cfg.AllowedOrigins) > 0 {
		h = withCORS(s.cfg.AllowedOrigins, h)
	}
	h = withRecovery(h)
	h = withTraceID(h)
	h = withRequestID(h)
	return h
}

type Principal struct {
	Sub           string
	Admin         bool
	AllowedAgents map[string]bool
	AuthKind      string
}

type relayOutcome struct {
	BrokerStatus int
	AgentStatus  int
	Headers      map[string]string
	Body         []byte
	Err          string
	LatencyMS    int
}

func (s *Server) auditRelay(ctx context.Context, p *Principal, agentID, method, agentPath string, status, latencyMS int, errStr string) {
	if p == nil {
		return
	}
	tid := traceIDFromContext(ctx)
	_ = s.cfg.DB.InsertRelayAudit(ctx, p.Sub, agentID, method, agentPath, status, latencyMS, errStr, tid)
	s.cfg.Events.PublishTo([]string{p.Sub}, events.Event{
		Type:    "relay_audit",
		AgentID: agentID,
		UserSub: p.Sub,
		TraceID: tid,
		Payload: map[string]any{
			"method":     method,
			"path":       agentPath,
			"status":     status,
			"latency_ms": latencyMS,
			"error":      errStr,
		},
	})
}

func (s *Server) relayAgentHTTP(ctx context.Context, p *Principal, agentID, method, agentPath, rawQuery string, headers map[string]string, body []byte) relayOutcome {
	start := time.Now()
	out := relayOutcome{
		Headers: map[string]string{},
	}

	a, err := s.cfg.Registry.Require(agentID)
	if err != nil {
		out.BrokerStatus = http.StatusBadGateway
		out.Err = "agent not connected"
		out.LatencyMS = int(time.Since(start).Milliseconds())
		s.auditRelay(ctx, p, agentID, method, agentPath, 0, out.LatencyMS, out.Err)
		return out
	}

	reqID := newID()
	ch, err := a.RegisterPending(reqID)
	if err != nil {
		out.BrokerStatus = http.StatusServiceUnavailable
		out.Err = "broker overloaded"
		out.LatencyMS = int(time.Since(start).Milliseconds())
		s.auditRelay(ctx, p, agentID, method, agentPath, 0, out.LatencyMS, out.Err)
		return out
	}
	defer a.UnregisterPending(reqID)

	msg := proto.RelayRequest{
		Type: proto.TypeHTTPRequest,
		ID:   reqID,
		Req: proto.HTTPRequest{
			Method:  method,
			Path:    agentPath,
			Query:   rawQuery,
			Headers: headers,
			BodyB64: base64.StdEncoding.EncodeToString(body),
		},
	}

	if err := a.Send(msg); err != nil {
		s.cfg.Registry.Delete(agentID)
		out.BrokerStatus = http.StatusBadGateway
		out.Err = "agent send failed"
		out.LatencyMS = int(time.Since(start).Milliseconds())
		s.auditRelay(ctx, p, agentID, method, agentPath, 0, out.LatencyMS, out.Err)
		return out
	}

	timeout := 60 * time.Second
	if dl, ok := ctx.Deadline(); ok {
		timeout = time.Until(dl)
	}
	if timeout < 100*time.Millisecond {
		timeout = 100 * time.Millisecond
	}
	timer := time.NewTimer(timeout)
	defer timer.Stop()

	select {
	case resp, ok := <-ch:
		if !ok {
			out.BrokerStatus = http.StatusBadGateway
			out.Err = "agent disconnected"
			out.LatencyMS = int(time.Since(start).Milliseconds())
			s.auditRelay(ctx, p, agentID, method, agentPath, 0, out.LatencyMS, out.Err)
			return out
		}
		if resp.Err != "" {
			out.BrokerStatus = http.StatusBadGateway
			out.Err = resp.Err
			out.LatencyMS = int(time.Since(start).Milliseconds())
			s.auditRelay(ctx, p, agentID, method, agentPath, 0, out.LatencyMS, out.Err)
			return out
		}

		out.AgentStatus = resp.Resp.Status
		for k, v := range resp.Resp.Headers {
			kl := strings.ToLower(k)
			if kl == "content-length" || kl == "connection" || kl == "transfer-encoding" {
				continue
			}
			out.Headers[k] = v
		}
		b, err := base64.StdEncoding.DecodeString(resp.Resp.BodyB64)
		if err != nil {
			out.BrokerStatus = http.StatusBadGateway
			out.Err = "invalid agent body"
			out.LatencyMS = int(time.Since(start).Milliseconds())
			s.auditRelay(ctx, p, agentID, method, agentPath, out.AgentStatus, out.LatencyMS, out.Err)
			return out
		}
		out.Body = b
		out.LatencyMS = int(time.Since(start).Milliseconds())
		s.auditRelay(ctx, p, agentID, method, agentPath, out.AgentStatus, out.LatencyMS, "")
		return out

	case <-timer.C:
		out.BrokerStatus = http.StatusGatewayTimeout
		out.Err = "agent timeout"
		out.LatencyMS = int(time.Since(start).Milliseconds())
		s.auditRelay(ctx, p, agentID, method, agentPath, 0, out.LatencyMS, out.Err)
		return out
	}
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
	if p.AuthKind != "oidc" {
		http.Error(w, "oidc required for agent listing", http.StatusForbidden)
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
	if p.AuthKind != "oidc" {
		http.Error(w, "oidc required for agent creation", http.StatusForbidden)
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
	hasHeader := func(name string) bool {
		for k := range headers {
			if strings.EqualFold(k, name) {
				return true
			}
		}
		return false
	}
	if !hasHeader("X-Request-ID") {
		if rid := requestIDFromContext(r.Context()); rid != "" {
			headers["X-Request-ID"] = rid
		}
	}
	if !hasHeader("X-Trace-ID") {
		if tid := traceIDFromContext(r.Context()); tid != "" {
			headers["X-Trace-ID"] = tid
		}
	}
	headers["X-Agentd-Broker-User"] = p.Sub
	// Allow callers to pass through an agentd auth header without colliding with broker OIDC auth.
	// The broker consumes `Authorization: Bearer <oidc_jwt>`; the forwarded request may need a different
	// bearer token for the agent endpoint behind the connector.
	if v := strings.TrimSpace(r.Header.Get("X-Agentd-Authorization")); v != "" {
		headers["Authorization"] = v
		delete(headers, "X-Agentd-Authorization")
	}

	ro := s.relayAgentHTTP(r.Context(), p, agentID, r.Method, agentPath, r.URL.RawQuery, headers, body)
	if ro.BrokerStatus != 0 {
		http.Error(w, ro.Err, ro.BrokerStatus)
		return
	}
	for k, v := range ro.Headers {
		w.Header().Set(k, v)
	}
	w.WriteHeader(ro.AgentStatus)
	_, _ = w.Write(ro.Body)
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
	hasHeader := func(name string) bool {
		for k := range headers {
			if strings.EqualFold(k, name) {
				return true
			}
		}
		return false
	}
	if !hasHeader("X-Request-ID") {
		if rid := requestIDFromContext(r.Context()); rid != "" {
			headers["X-Request-ID"] = rid
		}
	}
	if !hasHeader("X-Trace-ID") {
		if tid := traceIDFromContext(r.Context()); tid != "" {
			headers["X-Trace-ID"] = tid
		}
	}
	headers["X-Agentd-Broker-User"] = p.Sub
	if v := strings.TrimSpace(r.Header.Get("X-Agentd-Authorization")); v != "" {
		headers["Authorization"] = v
		delete(headers, "X-Agentd-Authorization")
	}

	streamID := newID()
	streamCh, err := a.RegisterStream(streamID)
	if err != nil {
		http.Error(w, "broker overloaded", http.StatusServiceUnavailable)
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
	// Detect dead peers. The connector should answer pings; the read deadline is extended on pong.
	_ = conn.SetReadDeadline(time.Now().Add(120 * time.Second))
	conn.SetPongHandler(func(string) error {
		_ = conn.SetReadDeadline(time.Now().Add(120 * time.Second))
		return nil
	})

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
		PendingLimit: func() int {
			if s.cfg.MaxPendingPerAgent < 0 {
				return 0
			}
			return s.cfg.MaxPendingPerAgent
		}(),
		StreamLimit: func() int {
			if s.cfg.MaxStreamsPerAgent < 0 {
				return 0
			}
			return s.cfg.MaxStreamsPerAgent
		}(),
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

	// Keep agent websocket alive and surface dead connections promptly.
	go func() {
		t := time.NewTicker(30 * time.Second)
		defer t.Stop()
		for {
			select {
			case <-ac.Done():
				return
			case <-t.C:
				if err := ac.Ping(); err != nil {
					ac.Close()
					return
				}
			}
		}
	}()

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
			if ok := a.DeliverStream(st.ID, st); !ok {
				_ = a.SendAny(proto.StreamCancel{Type: proto.TypeHTTPStreamCancel, ID: st.ID})
			}
		case proto.TypeHTTPStreamChunk:
			var ch proto.StreamChunk
			if err := json.Unmarshal(raw, &ch); err != nil {
				continue
			}
			if ok := a.DeliverStream(ch.ID, ch); !ok {
				_ = a.SendAny(proto.StreamCancel{Type: proto.TypeHTTPStreamCancel, ID: ch.ID})
			}
		case proto.TypeHTTPStreamEnd:
			var en proto.StreamEnd
			if err := json.Unmarshal(raw, &en); err != nil {
				continue
			}
			_ = a.DeliverStream(en.ID, en)
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
	if s.cfg.OIDC != nil {
		pr, err := s.cfg.OIDC.AuthenticateRequest(r.Context(), r)
		if err == nil {
			p := &Principal{
				Sub:      pr.Sub,
				Admin:    s.cfg.AdminSubs != nil && s.cfg.AdminSubs[pr.Sub],
				AuthKind: "oidc",
			}
			if err := s.cfg.DB.EnsureUser(r.Context(), p.Sub); err != nil {
				return nil, err
			}
			return p, nil
		}
		if s.getClientAuth() == nil || !s.cfg.ClientAuthFallback {
			return nil, err
		}
	}

	ca := s.getClientAuth()
	if ca == nil {
		return nil, errors.New("client auth not configured")
	}
	cp, err := ca.AuthenticateBearer(r)
	if err != nil {
		return nil, err
	}
	sub := strings.TrimSpace(cp.ClientID)
	if sub == "" {
		sub = "client"
	}
	if !strings.HasPrefix(sub, "client:") {
		sub = "client:" + sub
	}
	return &Principal{
		Sub:           sub,
		Admin:         cp.Admin,
		AllowedAgents: cp.AllowedAgents,
		AuthKind:      "client",
	}, nil
}

func (s *Server) canAccessAgent(ctx context.Context, p *Principal, agentID string) (bool, error) {
	if p == nil {
		return false, nil
	}
	if p.Admin {
		return true, nil
	}
	if p.AllowedAgents != nil {
		return p.AllowedAgents[agentID], nil
	}
	return s.cfg.DB.UserCanAccessAgent(ctx, p.Sub, agentID)
}

func (s *Server) handleAgentDelete(w http.ResponseWriter, r *http.Request, agentID string) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		http.Error(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if p.AuthKind == "client" && !p.Admin {
		http.Error(w, "client auth cannot delete agents", http.StatusForbidden)
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
	// Helps Nginx/Envoy deployments keep SSE streaming rather than buffering.
	w.Header().Set("X-Accel-Buffering", "no")

	ch, cancel := s.cfg.Events.Subscribe(p.Sub)
	defer cancel()

	_, _ = w.Write([]byte(":ok\n\n"))
	fl.Flush()

	keepAlive := s.cfg.SSEKeepaliveInterval
	if keepAlive <= 0 {
		keepAlive = 15 * time.Second
	}
	t := time.NewTicker(keepAlive)
	defer t.Stop()

	for {
		select {
		case <-r.Context().Done():
			return
		case <-t.C:
			_, _ = w.Write([]byte(":keepalive\n\n"))
			fl.Flush()
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

func (s *Server) handleHealthz(w http.ResponseWriter, r *http.Request) {
	_ = r
	writeJSON(w, map[string]any{
		"ok":         true,
		"ts_unix_ms": time.Now().UnixMilli(),
	})
}

func (s *Server) handleReadyz(w http.ResponseWriter, r *http.Request) {
	ok, errStr := s.checkReady(r.Context())
	if !ok {
		w.WriteHeader(http.StatusServiceUnavailable)
		writeJSON(w, map[string]any{
			"ok":         false,
			"error":      errStr,
			"ts_unix_ms": time.Now().UnixMilli(),
		})
		return
	}
	writeJSON(w, map[string]any{
		"ok":         true,
		"ts_unix_ms": time.Now().UnixMilli(),
	})
}

func (s *Server) handleMetrics(w http.ResponseWriter, r *http.Request) {
	_ = r
	readyOK, _ := s.checkReady(r.Context())
	ready := 0
	if readyOK {
		ready = 1
	}
	agents := 0
	if s != nil && s.cfg.Registry != nil {
		agents = len(s.cfg.Registry.List())
	}
	clientAuthConfigured := 0
	if s != nil && s.getClientAuth() != nil {
		clientAuthConfigured = 1
	}
	clientAuthOK, clientAuthAt, _ := s.getClientAuthStatus()
	clientAuthOKVal := 0
	if clientAuthOK {
		clientAuthOKVal = 1
	}
	clientAuthStrict := 0
	if s != nil && s.cfg.ClientAuthStrict {
		clientAuthStrict = 1
	}
	clientAuthAtMs := int64(0)
	if !clientAuthAt.IsZero() {
		clientAuthAtMs = clientAuthAt.UnixMilli()
	}
	uptime := time.Since(s.startTime).Seconds()
	nowMs := time.Now().UnixMilli()

	w.Header().Set("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
	_, _ = fmt.Fprintf(w, "# HELP broker_up 1 if the broker process is running.\n")
	_, _ = fmt.Fprintf(w, "# TYPE broker_up gauge\n")
	_, _ = fmt.Fprintf(w, "broker_up 1\n")
	_, _ = fmt.Fprintf(w, "# HELP broker_ready 1 if the broker is ready to serve traffic.\n")
	_, _ = fmt.Fprintf(w, "# TYPE broker_ready gauge\n")
	_, _ = fmt.Fprintf(w, "broker_ready %d\n", ready)
	_, _ = fmt.Fprintf(w, "# HELP broker_client_auth_configured 1 if client auth is configured.\n")
	_, _ = fmt.Fprintf(w, "# TYPE broker_client_auth_configured gauge\n")
	_, _ = fmt.Fprintf(w, "broker_client_auth_configured %d\n", clientAuthConfigured)
	_, _ = fmt.Fprintf(w, "# HELP broker_client_auth_last_reload_ok 1 if last client auth reload succeeded.\n")
	_, _ = fmt.Fprintf(w, "# TYPE broker_client_auth_last_reload_ok gauge\n")
	_, _ = fmt.Fprintf(w, "broker_client_auth_last_reload_ok %d\n", clientAuthOKVal)
	_, _ = fmt.Fprintf(w, "# HELP broker_client_auth_strict 1 if client auth strict mode is enabled.\n")
	_, _ = fmt.Fprintf(w, "# TYPE broker_client_auth_strict gauge\n")
	_, _ = fmt.Fprintf(w, "broker_client_auth_strict %d\n", clientAuthStrict)
	_, _ = fmt.Fprintf(w, "# HELP broker_client_auth_last_reload_unix_ms Unix ms for last client auth reload (0 if never).\n")
	_, _ = fmt.Fprintf(w, "# TYPE broker_client_auth_last_reload_unix_ms gauge\n")
	_, _ = fmt.Fprintf(w, "broker_client_auth_last_reload_unix_ms %d\n", clientAuthAtMs)
	_, _ = fmt.Fprintf(w, "# HELP broker_agents_connected Number of connected agents.\n")
	_, _ = fmt.Fprintf(w, "# TYPE broker_agents_connected gauge\n")
	_, _ = fmt.Fprintf(w, "broker_agents_connected %d\n", agents)
	_, _ = fmt.Fprintf(w, "# HELP broker_uptime_seconds Process uptime in seconds.\n")
	_, _ = fmt.Fprintf(w, "# TYPE broker_uptime_seconds gauge\n")
	_, _ = fmt.Fprintf(w, "broker_uptime_seconds %.0f\n", uptime)
	_, _ = fmt.Fprintf(w, "# HELP broker_now_unix_ms Current unix time in milliseconds.\n")
	_, _ = fmt.Fprintf(w, "# TYPE broker_now_unix_ms gauge\n")
	_, _ = fmt.Fprintf(w, "broker_now_unix_ms %d\n", nowMs)
}

func (s *Server) checkReady(ctx context.Context) (bool, string) {
	if s == nil {
		return false, "nil server"
	}
	cacheFor := s.cfg.ReadinessCacheInterval
	if cacheFor <= 0 {
		cacheFor = 5 * time.Second
	}

	now := time.Now()
	s.readyMu.Lock()
	if !s.readyAt.IsZero() && now.Sub(s.readyAt) < cacheFor {
		ok := s.readyOK
		errStr := s.readyErr
		s.readyMu.Unlock()
		return ok, errStr
	}
	// Avoid a thundering herd of readiness checks.
	if s.readyBusy {
		ok := s.readyOK
		errStr := s.readyErr
		s.readyMu.Unlock()
		return ok, errStr
	}
	s.readyBusy = true
	s.readyMu.Unlock()

	ok := true
	errStr := ""

	// DB health.
	if s.cfg.DB == nil || s.cfg.DB.Pool == nil {
		ok = false
		errStr = "db not configured"
	} else {
		pctx, cancel := context.WithTimeout(ctx, 2*time.Second)
		if err := s.cfg.DB.Pool.Ping(pctx); err != nil {
			ok = false
			errStr = "db ping failed: " + err.Error()
		}
		cancel()
	}

	// OIDC readiness (issuer/JWKS reachable) if configured.
	if ok {
		if s.cfg.OIDC == nil {
			ca := s.getClientAuth()
			if ca == nil {
				ok = false
				errStr = "oidc verifier not configured"
			} else if s.cfg.ClientAuthStrict {
				lastOK, _, lastErr := s.getClientAuthStatus()
				if !lastOK {
					ok = false
					if strings.TrimSpace(lastErr) != "" {
						errStr = "client auth reload failed: " + lastErr
					} else {
						errStr = "client auth reload failed"
					}
				}
			}
		} else {
			octx, cancel := context.WithTimeout(ctx, 3*time.Second)
			if err := s.cfg.OIDC.Init(octx); err != nil {
				ok = false
				errStr = "oidc init failed: " + err.Error()
			}
			cancel()
		}
	}

	s.readyMu.Lock()
	s.readyBusy = false
	s.readyAt = now
	s.readyOK = ok
	s.readyErr = errStr
	s.readyMu.Unlock()

	return ok, errStr
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
