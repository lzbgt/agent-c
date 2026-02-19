package broker

import (
	"errors"
	"net/http"
	"strings"
	"sync"
	"time"

	"agentd-broker/internal/auth"
	"agentd-broker/internal/db"
	"agentd-broker/internal/events"
	"agentd-broker/internal/oidc"
	"agentd-broker/internal/registry"

	"github.com/gorilla/websocket"
)

type Config struct {
	OIDC               *oidc.Verifier
	ClientAuth         *auth.ClientAuth
	ClientAuthFallback bool
	ClientAuthStrict   bool
	ClientAuthMaxAge   time.Duration
	DB                 *db.DB
	Registry           *registry.Registry
	Events             *events.Hub

	AgentCNPfx              string
	RequireAgentMTLS        bool
	MaxRequestBodySize      int64
	MaxPendingPerAgent      int
	MaxStreamsPerAgent      int
	IdempotencyTTL          time.Duration
	IdempotencyMaxBodyBytes int64

	// Cookie-based auth support (optional).
	AuthCookieName string

	// CORS for browser clients.
	// Origins may be exact (e.g. https://example.com), "*", or "re:<regex>".
	CorsOrigins          []string
	CorsAllowHeaders     string
	CorsAllowMethods     string
	CorsAllowCredentials bool
	CorsMaxAgeSeconds    int
	CorsRoutes           []CorsRoute

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
	clientAuthReloadMu  sync.RWMutex
	clientAuthReload    func(reason string) error
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
	if cfg.IdempotencyTTL < 0 {
		cfg.IdempotencyTTL = 0
	}
	if cfg.IdempotencyTTL == 0 {
		cfg.IdempotencyTTL = 24 * time.Hour
	}
	if cfg.IdempotencyMaxBodyBytes == 0 {
		cfg.IdempotencyMaxBodyBytes = 512 * 1024
	}
	if cfg.CorsMaxAgeSeconds == 0 {
		cfg.CorsMaxAgeSeconds = 600
	}
	if strings.TrimSpace(cfg.CorsAllowHeaders) == "" {
		cfg.CorsAllowHeaders = "Authorization, X-Agentd-Authorization, X-Agentd-Deployment, Content-Type, X-Request-ID, X-Trace-ID, Idempotency-Key, X-Idempotency-Key"
	}
	if strings.TrimSpace(cfg.CorsAllowMethods) == "" {
		cfg.CorsAllowMethods = "GET, POST, PUT, PATCH, DELETE, OPTIONS"
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
	wsOriginMatcher := buildOriginMatcher(cfg.CorsOrigins)
	s.upg = websocket.Upgrader{
		ReadBufferSize:  64 * 1024,
		WriteBufferSize: 64 * 1024,
		CheckOrigin: func(r *http.Request) bool {
			origin := strings.TrimSpace(r.Header.Get("Origin"))
			if origin == "" {
				return true
			}
			if len(cfg.CorsOrigins) == 0 {
				return true
			}
			ok, _, _ := wsOriginMatcher.match(origin)
			return ok
		},
	}
	return s, nil
}

func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/healthz", s.handleHealthz)
	mux.HandleFunc("/readyz", s.handleReadyz)
	mux.HandleFunc("/metrics", s.handleMetrics)
	mux.HandleFunc("/v1/caps", s.handleCaps)
	mux.HandleFunc("/v1/client_auth/status", s.handleClientAuthStatus)
	mux.HandleFunc("/v1/client_auth/reload", s.handleClientAuthReload)
	mux.HandleFunc("/v1/agent/connect", s.handleAgentConnect)
	mux.HandleFunc("/v1/agents", s.handleAgents)
	mux.HandleFunc("/v1/orchestrate", s.handleOrchestrate)
	mux.HandleFunc("/v1/trace", s.handleTrace)
	mux.HandleFunc("/v1/events", s.handleEventsSSE)
	// Catch-all for /v1/agents/{id}/... paths.
	mux.HandleFunc("/v1/agents/", s.handleAgentsSubroutes)
	h := http.Handler(mux)
	h = withAccessLog(h)
	h = withCORS(CorsConfig{
		Origins:          s.cfg.CorsOrigins,
		AllowHeaders:     s.cfg.CorsAllowHeaders,
		AllowMethods:     s.cfg.CorsAllowMethods,
		AllowCredentials: s.cfg.CorsAllowCredentials,
		MaxAgeSeconds:    s.cfg.CorsMaxAgeSeconds,
		Routes:           s.cfg.CorsRoutes,
	}, h)
	h = withRecovery(h)
	h = withTraceID(h)
	h = withRequestID(h)
	return h
}
