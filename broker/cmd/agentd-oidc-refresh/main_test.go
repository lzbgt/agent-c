package main

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestResolveTokenURLFromIssuer(t *testing.T) {
	var tokenURL string
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/.well-known/openid-configuration" {
			w.WriteHeader(http.StatusNotFound)
			return
		}
		payload := map[string]any{
			"token_endpoint": tokenURL,
		}
		_ = json.NewEncoder(w).Encode(payload)
	}))
	defer ts.Close()

	tokenURL = ts.URL + "/token"

	cfg := config{issuer: ts.URL, clientID: "client", username: "user", password: "pass"}
	got, err := resolveTokenURL(context.Background(), ts.Client(), &cfg)
	if err != nil {
		t.Fatalf("resolveTokenURL error: %v", err)
	}
	if got != tokenURL {
		t.Fatalf("expected token url %q, got %q", tokenURL, got)
	}
}

func TestResolveTokenURLFromKeycloakBase(t *testing.T) {
	cfg := config{keycloakBase: "http://example.com", realm: "agentd", clientID: "c", username: "u", password: "p"}
	got, err := resolveTokenURL(context.Background(), http.DefaultClient, &cfg)
	if err != nil {
		t.Fatalf("resolveTokenURL error: %v", err)
	}
	want := "http://example.com/realms/agentd/protocol/openid-connect/token"
	if got != want {
		t.Fatalf("expected token url %q, got %q", want, got)
	}
}

func TestResolveTokenURLIssuerMissingTokenEndpoint(t *testing.T) {
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_ = json.NewEncoder(w).Encode(map[string]any{"issuer": "noop"})
	}))
	defer ts.Close()

	cfg := config{issuer: ts.URL, clientID: "client", username: "user", password: "pass"}
	if _, err := resolveTokenURL(context.Background(), ts.Client(), &cfg); err == nil {
		t.Fatal("expected error for missing token_endpoint")
	}
}

func TestFetchTokenPasswordGrant(t *testing.T) {
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		vals, _ := url.ParseQuery(string(body))
		if vals.Get("grant_type") != "password" {
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		if vals.Get("username") != "user" || vals.Get("password") != "pass" {
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		if vals.Get("client_id") != "client" {
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		_ = json.NewEncoder(w).Encode(tokenResponse{AccessToken: "tok", ExpiresIn: 120, RefreshToken: "ref"})
	}))
	defer ts.Close()

	cfg := config{tokenURL: ts.URL, clientID: "client", username: "user", password: "pass"}
	resp, err := fetchToken(context.Background(), ts.Client(), &cfg)
	if err != nil {
		t.Fatalf("fetchToken error: %v", err)
	}
	if resp.AccessToken != "tok" {
		t.Fatalf("expected access token tok, got %q", resp.AccessToken)
	}
	if resp.RefreshToken != "ref" {
		t.Fatalf("expected refresh token ref, got %q", resp.RefreshToken)
	}
}

func TestFetchTokenRefreshGrant(t *testing.T) {
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		vals, _ := url.ParseQuery(string(body))
		if vals.Get("grant_type") != "refresh_token" {
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		if vals.Get("refresh_token") != "refresh123" {
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		_ = json.NewEncoder(w).Encode(tokenResponse{AccessToken: "tok2", ExpiresIn: 60})
	}))
	defer ts.Close()

	cfg := config{tokenURL: ts.URL, clientID: "client", refreshToken: "refresh123"}
	resp, err := fetchToken(context.Background(), ts.Client(), &cfg)
	if err != nil {
		t.Fatalf("fetchToken error: %v", err)
	}
	if resp.AccessToken != "tok2" {
		t.Fatalf("expected access token tok2, got %q", resp.AccessToken)
	}
}

func TestFetchTokenClientCredentialsGrant(t *testing.T) {
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		vals, _ := url.ParseQuery(string(body))
		if vals.Get("grant_type") != "client_credentials" {
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		if vals.Get("client_id") != "client" || vals.Get("client_secret") != "secret" {
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		_ = json.NewEncoder(w).Encode(tokenResponse{AccessToken: "tok3", ExpiresIn: 30})
	}))
	defer ts.Close()

	cfg := config{tokenURL: ts.URL, clientID: "client", clientSecret: "secret"}
	resp, err := fetchToken(context.Background(), ts.Client(), &cfg)
	if err != nil {
		t.Fatalf("fetchToken error: %v", err)
	}
	if resp.AccessToken != "tok3" {
		t.Fatalf("expected access token tok3, got %q", resp.AccessToken)
	}
}

func TestWriteTokenFile(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "token.txt")
	if err := writeTokenFile(path, "abc123"); err != nil {
		t.Fatalf("writeTokenFile error: %v", err)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read token file: %v", err)
	}
	if strings.TrimSpace(string(data)) != "abc123" {
		t.Fatalf("expected token abc123, got %q", strings.TrimSpace(string(data)))
	}
}
