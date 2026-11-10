// Package identity is the product-server client for Hubstack's shared OAuth
// broker. Never embed the application credential in a mobile app or browser.
package identity

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"
)

var ErrRejected = errors.New("shared sign in rejected")
var ErrUnavailable = errors.New("shared sign in unavailable")

// Rejection preserves a safe HTTP status for callers that must distinguish a
// stale user grant (403) from unavailable product credentials (401) or readiness
// (409). It deliberately retains no response body, token or provider details.
type Rejection struct{ Status int }

func (e *Rejection) Error() string { return ErrRejected.Error() }
func (e *Rejection) Unwrap() error { return ErrRejected }

type Client struct {
	origin, token string
	http          *http.Client
}
type Start struct {
	AuthorizationURL string    `json:"authorization_url"`
	ExpiresAt        time.Time `json:"expires_at"`
}
type Proof struct {
	Provider  string `json:"provider"`
	Namespace string `json:"namespace"`
	Subject   string `json:"subject"`
}
type Account struct {
	ID       string `json:"id"`
	Revision int64  `json:"revision"`
}
type Session struct {
	Account   Account   `json:"account"`
	ExpiresAt time.Time `json:"expires_at"`
}
type Result struct {
	Purpose      string    `json:"purpose"`
	Account      Account   `json:"account"`
	SessionToken string    `json:"session_token"`
	ExpiresAt    time.Time `json:"expires_at"`
	Identity     Proof     `json:"identity"`
	State        string    `json:"state"`
}

func New(origin, credential string) (*Client, error) {
	u, err := url.Parse(origin)
	raw, keyErr := hex.DecodeString(credential)
	if err != nil || u.Scheme != "https" || u.Hostname() == "" || u.Path != "" || u.RawQuery != "" || u.ForceQuery || u.Fragment != "" || u.User != nil || u.Opaque != "" || keyErr != nil || len(raw) != 32 || strings.ToLower(credential) != credential {
		return nil, ErrRejected
	}
	return &Client{origin: origin, token: credential, http: &http.Client{Timeout: 15 * time.Second, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}}, nil
}
func token(v string) bool {
	b, err := base64.RawURLEncoding.Strict().DecodeString(v)
	return len(v) == 43 && err == nil && len(b) == 32
}
func (c *Client) request(ctx context.Context, method, path string, body any, out any) error {
	var raw []byte
	if body != nil {
		var err error
		raw, err = json.Marshal(body)
		if err != nil || len(raw) > 8192 {
			return ErrRejected
		}
	}
	req, err := http.NewRequestWithContext(ctx, method, c.origin+path, bytes.NewReader(raw))
	if err != nil {
		return ErrRejected
	}
	req.Header.Set("Authorization", "Bearer "+c.token)
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Accept", "application/json")
	res, err := c.http.Do(req)
	if err != nil {
		return ErrUnavailable
	}
	defer res.Body.Close()
	if res.StatusCode != 200 && res.StatusCode != 201 {
		if res.StatusCode >= 400 && res.StatusCode < 500 && res.StatusCode != 429 {
			return &Rejection{Status: res.StatusCode}
		}
		return ErrUnavailable
	}
	data, err := io.ReadAll(io.LimitReader(res.Body, 8193))
	if err != nil || len(data) > 8192 || json.Unmarshal(data, out) != nil {
		return ErrUnavailable
	}
	return nil
}
func (c *Client) Providers(ctx context.Context) (map[string]bool, error) {
	var providers map[string]bool
	if err := c.request(ctx, "GET", "/v1/identity/providers", nil, &providers); err != nil {
		return nil, err
	}
	for name := range providers {
		if !provider(name) {
			return nil, ErrUnavailable
		}
	}
	return providers, nil
}
func provider(v string) bool { return v == "google" || v == "github" || v == "apple" }
func (c *Client) Begin(ctx context.Context, name, redirect, state, verifier string) (Start, error) {
	return c.begin(ctx, name, redirect, state, verifier, "")
}
func (c *Client) BeginLink(ctx context.Context, name, redirect, state, verifier, sessionToken string) (Start, error) {
	if !hexToken(sessionToken, 32) {
		return Start{}, ErrRejected
	}
	return c.begin(ctx, name, redirect, state, verifier, sessionToken)
}
func (c *Client) begin(ctx context.Context, name, redirect, state, verifier, linkSession string) (Start, error) {
	var out Start
	u, err := url.Parse(redirect)
	if !provider(name) || !token(state) || !token(verifier) || err != nil || u.Scheme != "https" || u.Hostname() == "" || u.Path == "" || u.User != nil || u.RawQuery != "" || u.ForceQuery || u.Fragment != "" || u.RawPath != "" {
		return out, ErrRejected
	}
	sum := sha256.Sum256([]byte(verifier))
	body := map[string]string{"provider": name, "redirect_uri": redirect, "state": state, "code_challenge": base64.RawURLEncoding.EncodeToString(sum[:])}
	if linkSession != "" {
		body["link_session"] = linkSession
	}
	if err = c.request(ctx, "POST", "/v1/identity/attempts", body, &out); err != nil {
		return Start{}, err
	}
	prefix := c.origin + "/oauth/authorize/"
	if !strings.HasPrefix(out.AuthorizationURL, prefix) || !token(strings.TrimPrefix(out.AuthorizationURL, prefix)) || !out.ExpiresAt.After(time.Now()) || out.ExpiresAt.After(time.Now().Add(11*time.Minute)) {
		return Start{}, ErrUnavailable
	}
	return out, nil
}
func subject(v string) bool {
	if len(v) < 1 || len(v) > 255 {
		return false
	}
	for _, b := range []byte(v) {
		if b < 33 || b > 126 {
			return false
		}
	}
	return true
}

// Exchange is one-use. An ambiguous response requires a fresh user sign-in;
// clients must not automatically retry it or turn email into an account key.
func (c *Client) Exchange(ctx context.Context, code, verifier, expectedProvider, expectedNamespace, expectedState string) (Result, error) {
	return c.exchange(ctx, code, verifier, expectedProvider, expectedNamespace, expectedState, "login", "")
}
func (c *Client) ExchangeLink(ctx context.Context, code, verifier, expectedProvider, expectedNamespace, expectedState, expectedAccount string) (Result, error) {
	if !accountID(expectedAccount) {
		return Result{}, ErrRejected
	}
	return c.exchange(ctx, code, verifier, expectedProvider, expectedNamespace, expectedState, "link", expectedAccount)
}
func hexToken(value string, size int) bool {
	raw, err := hex.DecodeString(value)
	return err == nil && len(raw) == size && strings.ToLower(value) == value
}
func accountID(value string) bool {
	return strings.HasPrefix(value, "usr_") && hexToken(strings.TrimPrefix(value, "usr_"), 16)
}
func validSession(account Account, expires time.Time) bool {
	return accountID(account.ID) && account.Revision > 0 && expires.After(time.Now()) && expires.Before(time.Now().Add(30*24*time.Hour+time.Minute))
}
func (c *Client) exchange(ctx context.Context, code, verifier, expectedProvider, expectedNamespace, expectedState, purpose, account string) (Result, error) {
	var out Result
	if !hexToken(code, 32) || !token(verifier) || !token(expectedState) || !provider(expectedProvider) || !subject(expectedNamespace) {
		return Result{}, ErrRejected
	}
	if err := c.request(ctx, "POST", "/v1/identity/exchange", map[string]string{"code": code, "verifier": verifier}, &out); err != nil {
		return Result{}, err
	}
	if out.State != expectedState || out.Identity.Provider != expectedProvider || out.Identity.Namespace != expectedNamespace || !subject(out.Identity.Subject) || out.Purpose != purpose || !hexToken(out.SessionToken, 32) || !validSession(out.Account, out.ExpiresAt) || account != "" && account != out.Account.ID {
		return Result{}, ErrRejected
	}
	return out, nil
}

// Session validates a product's server-held shared session, never a browser identity.
func (c *Client) Session(ctx context.Context, sessionToken, expectedAccount string) (Session, error) {
	var out Session
	if !hexToken(sessionToken, 32) || !accountID(expectedAccount) {
		return out, ErrRejected
	}
	if err := c.request(ctx, "POST", "/v1/identity/session", map[string]string{"session_token": sessionToken}, &out); err != nil {
		return Session{}, err
	}
	if !validSession(out.Account, out.ExpiresAt) || out.Account.ID != expectedAccount {
		return Session{}, ErrRejected
	}
	return out, nil
}
func (c *Client) Logout(ctx context.Context, sessionToken string) error {
	if !hexToken(sessionToken, 32) {
		return ErrRejected
	}
	var out struct {
		Revoked bool `json:"revoked"`
	}
	if err := c.request(ctx, "POST", "/v1/identity/logout", map[string]string{"session_token": sessionToken}, &out); err != nil {
		return err
	}
	if !out.Revoked {
		return ErrUnavailable
	}
	return nil
}
