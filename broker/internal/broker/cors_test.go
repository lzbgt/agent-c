package broker

import (
	"net/http"
	"net/http/httptest"
	"testing"
)

func TestCORSDefaultOriginMatch(t *testing.T) {
	cfg := CorsConfig{
		Origins:          []string{"https://example.com"},
		AllowHeaders:     "Authorization",
		AllowMethods:     "GET, POST",
		AllowCredentials: false,
		MaxAgeSeconds:    600,
	}
	called := false
	h := withCORS(cfg, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		called = true
		w.WriteHeader(http.StatusOK)
	}))
	req := httptest.NewRequest("GET", "http://broker/v1/agents", nil)
	req.Header.Set("Origin", "https://example.com")
	w := httptest.NewRecorder()
	h.ServeHTTP(w, req)
	if !called {
		t.Fatalf("handler not called")
	}
	if got := w.Header().Get("Access-Control-Allow-Origin"); got != "https://example.com" {
		t.Fatalf("unexpected allow-origin: %q", got)
	}
}

func TestCORSRoutePrecedence(t *testing.T) {
	cfg := CorsConfig{
		Origins:          []string{"https://default.example"},
		AllowHeaders:     "Authorization",
		AllowMethods:     "GET",
		AllowCredentials: false,
		MaxAgeSeconds:    600,
		Routes: []CorsRoute{
			{PathPrefix: "/v1/agents", Origins: []string{"https://agents.example"}},
			{PathPrefix: "/v1/agents/admin", Origins: []string{"https://admin.example"}},
		},
	}
	h := withCORS(cfg, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
	}))
	req := httptest.NewRequest("GET", "http://broker/v1/agents/admin/foo", nil)
	req.Header.Set("Origin", "https://admin.example")
	w := httptest.NewRecorder()
	h.ServeHTTP(w, req)
	if got := w.Header().Get("Access-Control-Allow-Origin"); got != "https://admin.example" {
		t.Fatalf("expected admin origin, got %q", got)
	}
}

func TestCORSAllowsCredentialsWithWildcard(t *testing.T) {
	cfg := CorsConfig{
		Origins:          []string{"*"},
		AllowHeaders:     "Authorization",
		AllowMethods:     "GET",
		AllowCredentials: true,
		MaxAgeSeconds:    600,
	}
	h := withCORS(cfg, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
	}))
	req := httptest.NewRequest("GET", "http://broker/v1/agents", nil)
	req.Header.Set("Origin", "https://foo.example")
	w := httptest.NewRecorder()
	h.ServeHTTP(w, req)
	if got := w.Header().Get("Access-Control-Allow-Origin"); got != "https://foo.example" {
		t.Fatalf("expected reflected origin, got %q", got)
	}
	if got := w.Header().Get("Access-Control-Allow-Credentials"); got != "true" {
		t.Fatalf("expected allow-credentials, got %q", got)
	}
}

func TestCORSOptionsRejectsUnknownOrigin(t *testing.T) {
	cfg := CorsConfig{Origins: []string{"https://example.com"}}
	h := withCORS(cfg, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
	}))
	req := httptest.NewRequest("OPTIONS", "http://broker/v1/agents", nil)
	req.Header.Set("Origin", "https://nope.example")
	w := httptest.NewRecorder()
	h.ServeHTTP(w, req)
	if w.Result().StatusCode != http.StatusForbidden {
		t.Fatalf("expected 403, got %d", w.Result().StatusCode)
	}
}
