package broker

import (
	"context"
	"errors"
	"net/http"
	"strings"
	"time"

	"agentd-broker/internal/auth"
)

type Principal struct {
	Sub           string
	Admin         bool
	AllowedAgents map[string]bool
	AuthKind      string
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

func (s *Server) SetClientAuthReload(fn func(reason string) error) {
	if s == nil {
		return
	}
	s.clientAuthReloadMu.Lock()
	s.clientAuthReload = fn
	s.clientAuthReloadMu.Unlock()
}

func (s *Server) reloadClientAuth(reason string) error {
	if s == nil {
		return errors.New("nil server")
	}
	s.clientAuthReloadMu.RLock()
	fn := s.clientAuthReload
	s.clientAuthReloadMu.RUnlock()
	if fn == nil {
		return errors.New("client auth reload not configured")
	}
	return fn(reason)
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
		if tok := authTokenFromCookie(r, s.cfg.AuthCookieName); tok != "" {
			if pr2, err2 := s.cfg.OIDC.AuthenticateRequest(r.Context(), requestWithBearer(r, tok)); err2 == nil {
				p := &Principal{
					Sub:      pr2.Sub,
					Admin:    s.cfg.AdminSubs != nil && s.cfg.AdminSubs[pr2.Sub],
					AuthKind: "oidc",
				}
				if err := s.cfg.DB.EnsureUser(r.Context(), p.Sub); err != nil {
					return nil, err
				}
				return p, nil
			}
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

func (s *Server) allowAutomationPrincipal(p *Principal) bool {
	if p == nil {
		return false
	}
	if p.AuthKind == "oidc" {
		return true
	}
	if p.AuthKind == "client" && p.Admin && s.cfg.ClientAuthAllowAutomation {
		return true
	}
	return false
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

func (s *Server) handleClientAuthStatus(w http.ResponseWriter, r *http.Request) {
	if r.Method != "GET" {
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if !p.Admin {
		writeErrorJSON(w, "admin required", http.StatusForbidden)
		return
	}
	ok, at, errStr := s.getClientAuthStatus()
	enabled := s.getClientAuth() != nil
	writeJSON(w, map[string]any{
		"ok":      true,
		"enabled": enabled,
		"last_ok": ok,
		"last_unix_ms": func() int64 {
			if at.IsZero() {
				return 0
			}
			return at.UnixMilli()
		}(),
		"last_error": errStr,
	})
}

func (s *Server) handleClientAuthReload(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if !p.Admin {
		writeErrorJSON(w, "admin required", http.StatusForbidden)
		return
	}
	if err := s.reloadClientAuth("api"); err != nil {
		writeErrorJSON(w, err.Error(), http.StatusBadRequest)
		return
	}
	writeJSON(w, map[string]any{"ok": true, "ts_unix_ms": time.Now().UnixMilli()})
}
