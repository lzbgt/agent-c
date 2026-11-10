package gateway

import (
	"context"
	"net/http"
	"strings"
	"time"
)

func (s *Server) authRoute(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path == "/auth/session" && r.Method == "GET" {
		account, admin, status := s.authenticate(r)
		if status == 401 {
			jsonResponse(w, 200, map[string]any{"authenticated": false})
			return
		}
		if status != 200 {
			fail(w, status, "Account verification is temporarily unavailable")
			return
		}
		data := map[string]any{"authenticated": true, "admin": admin, "account_id": account}
		if admin {
			data["display_name"] = s.cfg.AdminEmail
		}
		jsonResponse(w, 200, data)
		return
	}
	if r.URL.Path == "/auth/logout" && r.Method == "POST" {
		if !s.originOK(r, false) {
			fail(w, 403, "Request origin does not match this workspace")
			return
		}
		if c, err := r.Cookie(sessionCookie); err == nil {
			if s.db.DeleteSession(r.Context(), digest(c.Value)) != nil {
				fail(w, 503, "Cannot close this session")
				return
			}
		}
		setCookie(w, sessionCookie, "", -1)
		jsonResponse(w, 200, map[string]any{"ok": true})
		return
	}
	parts := strings.Split(strings.Trim(r.URL.Path, "/"), "/")
	if len(parts) != 3 || parts[0] != "auth" || providerNamespace(parts[1]) == "" || r.Method != "GET" {
		fail(w, 404, "Sign-in route not found")
		return
	}
	select {
	case s.oauthSlots <- struct{}{}:
		defer func() { <-s.oauthSlots }()
	default:
		fail(w, 429, "Sign-in is busy; try again shortly")
		return
	}
	if parts[2] == "start" {
		s.startLogin(w, r, parts[1])
		return
	}
	if parts[2] == "callback" {
		s.finishLogin(w, r, parts[1])
		return
	}
	fail(w, 404, "Sign-in route not found")
}
func (s *Server) startLogin(w http.ResponseWriter, r *http.Request, provider string) {
	state, e1 := randomToken()
	verifier, e2 := randomToken()
	browser, e3 := randomToken()
	if e1 != nil || e2 != nil || e3 != nil {
		fail(w, 503, "Cannot prepare sign-in")
		return
	}
	start, err := s.identity.Begin(r.Context(), provider, s.cfg.PublicURL+"/auth/"+provider+"/callback", state, verifier)
	if err != nil {
		fail(w, 503, "Cannot start sign-in; try again shortly")
		return
	}
	if s.db.AddAttempt(r.Context(), attempt{digest(state), digest(browser), provider, verifier, start.ExpiresAt}) != nil {
		fail(w, 503, "Cannot record sign-in")
		return
	}
	setCookie(w, attemptCookie, browser, 600)
	w.Header().Set("Cache-Control", "no-store")
	http.Redirect(w, r, start.AuthorizationURL, http.StatusSeeOther)
}
func (s *Server) finishLogin(w http.ResponseWriter, r *http.Request, provider string) {
	state, code := r.URL.Query().Get("state"), r.URL.Query().Get("code")
	c, err := r.Cookie(attemptCookie)
	if err != nil || !canonicalToken(c.Value) || !canonicalToken(state) || !hexID(code, 32) {
		fail(w, 400, "Sign-in expired or does not match this browser; start again")
		return
	}
	verifier, err := s.db.TakeAttempt(r.Context(), digest(state), digest(c.Value), provider)
	if err != nil {
		fail(w, 400, "Sign-in expired or was already used; start again")
		return
	}
	setCookie(w, attemptCookie, "", -1)
	result, err := s.identity.Exchange(r.Context(), code, verifier, provider, providerNamespace(provider), state)
	if err != nil {
		fail(w, 401, "Sign-in could not be completed; start again")
		return
	}
	defer func() {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = s.identity.Logout(ctx, result.SessionToken)
	}()
	account, err := s.identity.AccountState(r.Context(), result.Account.ID)
	if err != nil || account.ApplicationID != s.cfg.ApplicationID || account.InstanceID != s.cfg.InstanceID || account.AccountID != result.Account.ID || !account.Active || account.Revision != result.Account.Revision {
		fail(w, 503, "Cannot verify this workspace account")
		return
	}
	token, err := randomToken()
	if err != nil {
		fail(w, 503, "Cannot open session")
		return
	}
	if s.db.PutSession(r.Context(), session{digest(token), result.Account.ID, result.Account.Revision, time.Now().Add(12 * time.Hour)}) != nil {
		fail(w, 503, "Cannot save session")
		return
	}
	setCookie(w, sessionCookie, token, 12*3600)
	http.Redirect(w, r, s.cfg.PublicURL+"/", http.StatusSeeOther)
}
