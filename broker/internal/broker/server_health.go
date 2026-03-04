package broker

import (
	"context"
	"fmt"
	"net/http"
	"strings"
	"time"
)

func (s *Server) handleHealthz(w http.ResponseWriter, r *http.Request) {
	_ = r
	writeJSON(w, map[string]any{
		"ok":         true,
		"ts_unix_ms": time.Now().UnixMilli(),
	})
}

func (s *Server) handleReadyz(w http.ResponseWriter, r *http.Request) {
	ok, errStr := s.checkReady(r.Context())
	clientAuth := map[string]any(nil)
	if s.getClientAuth() != nil {
		lastOK, lastAt, lastErr := s.getClientAuthStatus()
		clientAuth = map[string]any{
			"enabled":    true,
			"last_ok":    lastOK,
			"strict":     s.cfg.ClientAuthStrict,
			"max_age_ms": s.cfg.ClientAuthMaxAge.Milliseconds(),
			"last_unix_ms": func() int64 {
				if lastAt.IsZero() {
					return 0
				}
				return lastAt.UnixMilli()
			}(),
		}
		if !lastOK && lastErr != "" {
			clientAuth["last_error"] = lastErr
		}
	}
	if !ok {
		w.WriteHeader(http.StatusServiceUnavailable)
		resp := errorEnvelope(errStr)
		resp["ts_unix_ms"] = time.Now().UnixMilli()
		resp["client_auth"] = func() any {
			if clientAuth == nil {
				return nil
			}
			return clientAuth
		}()
		writeJSON(w, resp)
		return
	}
	writeJSON(w, map[string]any{
		"ok":          true,
		"ts_unix_ms":  time.Now().UnixMilli(),
		"client_auth": clientAuth,
	})
}

func (s *Server) handleCaps(w http.ResponseWriter, r *http.Request) {
	_ = r
	now := time.Now()
	uptime := now.Sub(s.startTime)
	caEnabled := s.getClientAuth() != nil
	caps := map[string]any{
		"ok":          true,
		"service":     "broker",
		"version":     "0.1",
		"api_version": "v1",
		"now_unix_ms": now.UnixMilli(),
		"uptime_ms":   uptime.Milliseconds(),
		"features": map[string]any{
			"auth": map[string]any{
				"oidc_enabled":         s.cfg.OIDC != nil,
				"client_auth_enabled":  caEnabled,
				"client_auth_fallback": s.cfg.ClientAuthFallback,
				"client_auth_strict":   s.cfg.ClientAuthStrict,
				"cookie_enabled":       strings.TrimSpace(s.cfg.AuthCookieName) != "",
				"client_auth_max_age_ms": func() int64 {
					if s.cfg.ClientAuthMaxAge <= 0 {
						return 0
					}
					return s.cfg.ClientAuthMaxAge.Milliseconds()
				}(),
			},
			"client_prefs": map[string]any{
				"enabled":     true,
				"secrets":     false,
				"max_bytes":   clientPrefsMaxBodyBytes,
				"owner_scope": "oidc_sub",
			},
			"mTLS": map[string]any{
				"require_agent_mtls": s.cfg.RequireAgentMTLS,
				"agent_cn_prefix":    s.cfg.AgentCNPfx,
			},
			"events": map[string]any{
				"sse": true,
			},
			"connectors": map[string]any{
				"enabled": true,
				"count": func() int {
					if s.cfg.Connectors == nil {
						return 0
					}
					return s.cfg.Connectors.Count()
				}(),
			},
			"audio": map[string]any{
				"signaling": true,
				"mode":      "webrtc",
			},
			"idempotency": map[string]any{
				"enabled": s.cfg.IdempotencyTTL > 0,
			},
		},
		"limits": map[string]any{
			"max_request_body_bytes": s.cfg.MaxRequestBodySize,
			"max_pending_per_agent":  s.cfg.MaxPendingPerAgent,
			"max_streams_per_agent":  s.cfg.MaxStreamsPerAgent,
			"idempotency_ttl_ms": func() int64 {
				if s.cfg.IdempotencyTTL <= 0 {
					return 0
				}
				return s.cfg.IdempotencyTTL.Milliseconds()
			}(),
			"idempotency_max_body_bytes": s.cfg.IdempotencyMaxBodyBytes,
			"sse_keepalive_ms": func() int64 {
				if s.cfg.SSEKeepaliveInterval <= 0 {
					return 0
				}
				return s.cfg.SSEKeepaliveInterval.Milliseconds()
			}(),
			"readiness_cache_ms": func() int64 {
				if s.cfg.ReadinessCacheInterval <= 0 {
					return 0
				}
				return s.cfg.ReadinessCacheInterval.Milliseconds()
			}(),
			"audio_session_ttl_ms": func() int64 {
				if s.cfg.AudioSessionTTL <= 0 {
					return 0
				}
				return s.cfg.AudioSessionTTL.Milliseconds()
			}(),
		},
	}
	writeJSON(w, caps)
}

func (s *Server) handleMetrics(w http.ResponseWriter, r *http.Request) {
	_ = r
	readyOK, _ := s.checkReady(r.Context())
	ready := 0
	if readyOK {
		ready = 1
	}
	agents := 0
	deployments := 0
	if s != nil && s.cfg.Registry != nil {
		agents = s.cfg.Registry.AgentCount()
		deployments = s.cfg.Registry.ConnectionCount()
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
	clientAuthMaxAgeMs := int64(0)
	if s != nil && s.cfg.ClientAuthMaxAge > 0 {
		clientAuthMaxAgeMs = s.cfg.ClientAuthMaxAge.Milliseconds()
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
	_, _ = fmt.Fprintf(w, "# HELP broker_client_auth_max_age_ms Max age for client auth reload in ms (0 if disabled).\n")
	_, _ = fmt.Fprintf(w, "# TYPE broker_client_auth_max_age_ms gauge\n")
	_, _ = fmt.Fprintf(w, "broker_client_auth_max_age_ms %d\n", clientAuthMaxAgeMs)
	_, _ = fmt.Fprintf(w, "# HELP broker_client_auth_last_reload_unix_ms Unix ms for last client auth reload (0 if never).\n")
	_, _ = fmt.Fprintf(w, "# TYPE broker_client_auth_last_reload_unix_ms gauge\n")
	_, _ = fmt.Fprintf(w, "broker_client_auth_last_reload_unix_ms %d\n", clientAuthAtMs)
	_, _ = fmt.Fprintf(w, "# HELP broker_agents_connected Number of connected agents.\n")
	_, _ = fmt.Fprintf(w, "# TYPE broker_agents_connected gauge\n")
	_, _ = fmt.Fprintf(w, "broker_agents_connected %d\n", agents)
	_, _ = fmt.Fprintf(w, "# HELP broker_agent_deployments_connected Number of connected agent deployments.\n")
	_, _ = fmt.Fprintf(w, "# TYPE broker_agent_deployments_connected gauge\n")
	_, _ = fmt.Fprintf(w, "broker_agent_deployments_connected %d\n", deployments)
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
				lastOK, lastAt, lastErr := s.getClientAuthStatus()
				if !lastOK {
					ok = false
					if strings.TrimSpace(lastErr) != "" {
						errStr = "client auth reload failed: " + lastErr
					} else {
						errStr = "client auth reload failed"
					}
				} else if s.cfg.ClientAuthMaxAge > 0 {
					age := time.Since(lastAt)
					if lastAt.IsZero() || age > s.cfg.ClientAuthMaxAge {
						ok = false
						errStr = "client auth reload too old"
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
