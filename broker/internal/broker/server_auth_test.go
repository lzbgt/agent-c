package broker

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"agentd-broker/internal/auth"
)

func newTestAuthServer(token string) *Server {
	ca := &auth.ClientAuth{ByToken: map[string]*auth.ClientPolicy{}}
	ca.ByToken[token] = &auth.ClientPolicy{
		ClientID:      "test-client",
		Token:         token,
		Admin:         true,
		AllowedAgents: map[string]bool{"agent1": true},
	}
	s := &Server{
		cfg: Config{
			ClientAuth:     ca,
			AuthCookieName: "broker_auth",
		},
		startTime: time.Now(),
	}
	s.clientAuth = ca
	return s
}

func TestRequirePrincipalAcceptsClientAuthCookie(t *testing.T) {
	s := newTestAuthServer("test-token")
	req := httptest.NewRequest("GET", "http://broker/v1/agents", nil)
	req.AddCookie(&http.Cookie{Name: "broker_auth", Value: "test-token"})

	p, err := s.requirePrincipal(req)
	if err != nil {
		t.Fatalf("requirePrincipal: %v", err)
	}
	if p.AuthKind != "client" {
		t.Fatalf("expected client auth kind, got %q", p.AuthKind)
	}
	if p.Sub != "client:test-client" {
		t.Fatalf("expected client subject, got %q", p.Sub)
	}
	if !p.AllowedAgents["agent1"] {
		t.Fatalf("expected allowed agent from client policy")
	}
}

func TestHandleAuthSessionCreateSetsCookie(t *testing.T) {
	s := newTestAuthServer("test-token")
	req := httptest.NewRequest("POST", "http://broker/v1/auth/session", nil)
	req.Header.Set("Authorization", "Bearer test-token")
	req.Header.Set("X-Forwarded-Proto", "https")
	w := httptest.NewRecorder()

	s.handleAuthSession(w, req)

	res := w.Result()
	defer res.Body.Close()
	if res.StatusCode != http.StatusOK {
		t.Fatalf("expected 200, got %d", res.StatusCode)
	}
	cookies := res.Cookies()
	if len(cookies) != 1 {
		t.Fatalf("expected one cookie, got %d", len(cookies))
	}
	cookie := cookies[0]
	if cookie.Name != "broker_auth" {
		t.Fatalf("unexpected cookie name %q", cookie.Name)
	}
	if cookie.Value != "test-token" {
		t.Fatalf("unexpected cookie value %q", cookie.Value)
	}
	if !cookie.HttpOnly {
		t.Fatalf("expected HttpOnly cookie")
	}
	if !cookie.Secure {
		t.Fatalf("expected Secure cookie")
	}
	if cookie.SameSite != http.SameSiteNoneMode {
		t.Fatalf("expected SameSite=None, got %v", cookie.SameSite)
	}
	var body map[string]any
	if err := json.NewDecoder(res.Body).Decode(&body); err != nil {
		t.Fatalf("decode body: %v", err)
	}
	if ok, _ := body["ok"].(bool); !ok {
		t.Fatalf("expected ok response, got %#v", body)
	}
	if got, _ := body["auth_kind"].(string); got != "client" {
		t.Fatalf("expected client auth_kind, got %q", got)
	}
}

func TestHandleAuthSessionDeleteClearsCookie(t *testing.T) {
	s := newTestAuthServer("test-token")
	req := httptest.NewRequest("DELETE", "http://broker/v1/auth/session", nil)
	req.Header.Set("X-Forwarded-Proto", "https")
	w := httptest.NewRecorder()

	s.handleAuthSession(w, req)

	res := w.Result()
	defer res.Body.Close()
	if res.StatusCode != http.StatusOK {
		t.Fatalf("expected 200, got %d", res.StatusCode)
	}
	if setCookie := res.Header.Get("Set-Cookie"); !strings.Contains(setCookie, "broker_auth=") || !strings.Contains(setCookie, "Max-Age=0") {
		t.Fatalf("unexpected clear cookie header: %q", setCookie)
	}
	var body map[string]any
	if err := json.NewDecoder(res.Body).Decode(&body); err != nil {
		t.Fatalf("decode body: %v", err)
	}
	if cleared, _ := body["cleared"].(bool); !cleared {
		t.Fatalf("expected cleared response, got %#v", body)
	}
}
