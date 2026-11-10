package gateway

import (
	"context"
	"errors"
	shared "github.com/lzbgt/hubstack-payments/sdk/identity"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"strings"
	"sync"
	"testing"
	"time"
)

const ownerAccount = "usr_11111111111111111111111111111111"

type memoryStore struct {
	mu       sync.Mutex
	attempts map[string]attempt
	sessions map[string]session
}

func newMemoryStore() *memoryStore {
	return &memoryStore{attempts: map[string]attempt{}, sessions: map[string]session{}}
}
func (m *memoryStore) AddAttempt(_ context.Context, a attempt) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.attempts[a.StateHash] = a
	return nil
}
func (m *memoryStore) TakeAttempt(_ context.Context, state, browser, provider string) (string, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	a, ok := m.attempts[state]
	if !ok || a.BrowserHash != browser || a.Provider != provider || !a.Expires.After(time.Now()) {
		return "", errors.New("invalid attempt")
	}
	delete(m.attempts, state)
	return a.Verifier, nil
}
func (m *memoryStore) PutSession(_ context.Context, v session) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.sessions[v.TokenHash] = v
	return nil
}
func (m *memoryStore) Session(_ context.Context, id string) (session, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	v, ok := m.sessions[id]
	if !ok || !v.Expires.After(time.Now()) {
		return v, errors.New("missing")
	}
	return v, nil
}
func (m *memoryStore) DeleteSession(_ context.Context, id string) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	delete(m.sessions, id)
	return nil
}
func (m *memoryStore) Ping(context.Context) error { return nil }

type identityFixture struct {
	state, verifier    string
	account            shared.AccountState
	unavailable        bool
	exchanges, logouts int
}

func (f *identityFixture) Providers(context.Context) (map[string]bool, error) {
	return map[string]bool{"google": true, "github": true, "apple": true}, nil
}
func (f *identityFixture) Begin(_ context.Context, provider, redirect, state, verifier string) (shared.Start, error) {
	f.state = state
	f.verifier = verifier
	return shared.Start{AuthorizationURL: "https://payments.hubstack.cn/oauth/authorize/" + state, ExpiresAt: time.Now().Add(10 * time.Minute)}, nil
}
func (f *identityFixture) Exchange(_ context.Context, code, verifier, provider, namespace, state string) (shared.Result, error) {
	f.exchanges++
	if state != f.state || verifier != f.verifier {
		return shared.Result{}, errors.New("binding mismatch")
	}
	return shared.Result{Account: shared.Account{ID: f.account.AccountID, Revision: f.account.Revision}, Identity: shared.Proof{Provider: provider, Namespace: namespace, Subject: "verified-owner"}, SessionToken: strings.Repeat("a", 64)}, nil
}
func (f *identityFixture) AccountState(context.Context, string) (shared.AccountState, error) {
	if f.unavailable {
		return shared.AccountState{}, errors.New("offline")
	}
	return f.account, nil
}
func (f *identityFixture) Logout(context.Context, string) error { f.logouts++; return nil }
func fixture(t *testing.T) (*Server, *identityFixture, *memoryStore) {
	t.Helper()
	cfg := Config{PublicURL: "https://agent.example", ApplicationID: "agent-c", InstanceID: "instance", OperatorToken: strings.Repeat("b", 64), AdminAccountIDs: []string{ownerAccount}, AdminEmail: "owner@example.com"}
	f := &identityFixture{account: shared.AccountState{ApplicationID: cfg.ApplicationID, InstanceID: cfg.InstanceID, AccountID: ownerAccount, Active: true, Revision: 1}}
	db := newMemoryStore()
	s := &Server{cfg: cfg, identity: f, db: db, oauthSlots: make(chan struct{}, 4)}
	s.daemon = http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { w.WriteHeader(204) })
	return s, f, db
}
func request(s *Server, method, path string, cookie *http.Cookie, origin string) *httptest.ResponseRecorder {
	r := httptest.NewRequest(method, "https://agent.example"+path, nil)
	if cookie != nil {
		r.AddCookie(cookie)
	}
	if origin != "" {
		r.Header.Set("Origin", origin)
	}
	w := httptest.NewRecorder()
	s.ServeHTTP(w, r)
	return w
}
func login(t *testing.T, s *Server, f *identityFixture) *http.Cookie {
	t.Helper()
	beforeExchanges, beforeLogouts := f.exchanges, f.logouts
	start := request(s, "GET", "/auth/apple/start", nil, "")
	if start.Code != 303 {
		t.Fatal("start failed", start.Code)
	}
	cookies := start.Result().Cookies()
	if len(cookies) != 1 || !cookies[0].Secure || !cookies[0].HttpOnly || cookies[0].Path != "/" {
		t.Fatal("unsafe attempt cookie")
	}
	callback := "/auth/apple/callback?code=" + strings.Repeat("c", 64) + "&state=" + url.QueryEscape(f.state)
	wrong, _ := randomToken()
	bad := *cookies[0]
	bad.Value = wrong
	if request(s, "GET", callback, &bad, "").Code != 400 || f.exchanges != beforeExchanges {
		t.Fatal("foreign browser redeemed an attempt")
	}
	done := request(s, "GET", callback, cookies[0], "")
	if done.Code != 303 {
		t.Fatal("callback failed", done.Code, done.Body.String())
	}
	if request(s, "GET", callback, cookies[0], "").Code != 400 || f.exchanges != beforeExchanges+1 || f.logouts != beforeLogouts+1 {
		t.Fatal("callback was replayed or exchange grant leaked")
	}
	for _, c := range done.Result().Cookies() {
		if c.Name == sessionCookie {
			if !c.Secure || !c.HttpOnly || c.MaxAge != 12*3600 {
				t.Fatal("unsafe session cookie")
			}
			return c
		}
	}
	t.Fatal("missing session cookie")
	return nil
}
func TestLoginBindingRevocationAndScopedAdministration(t *testing.T) {
	s, f, db := fixture(t)
	if request(s, "GET", "/api/v1/config", nil, "").Code != 401 {
		t.Fatal("anonymous runtime access")
	}
	cookie := login(t, s, f)
	if request(s, "GET", "/api/v1/config", cookie, "").Code != 204 {
		t.Fatal("verified administrator rejected")
	}
	if request(s, "POST", "/api/v1/run", cookie, "").Code != 403 || request(s, "POST", "/api/v1/run", cookie, "https://foreign.example").Code != 403 {
		t.Fatal("CSRF admitted")
	}
	if request(s, "POST", "/api/v1/run", cookie, s.cfg.PublicURL).Code != 204 {
		t.Fatal("same-origin action rejected")
	}
	f.account.Revision = 2
	if request(s, "GET", "/api/v1/config", cookie, "").Code != 401 {
		t.Fatal("stale central revision admitted")
	}
	f.account.Revision = 1
	f.account.InstanceID = "other"
	if request(s, "GET", "/api/v1/config", cookie, "").Code != 401 {
		t.Fatal("foreign application instance admitted")
	}
	f.account.InstanceID = s.cfg.InstanceID
	f.unavailable = true
	if request(s, "GET", "/api/v1/config", cookie, "").Code != 503 {
		t.Fatal("identity outage failed open")
	}
	f.unavailable = false
	if request(s, "POST", "/auth/logout", cookie, s.cfg.PublicURL).Code != 200 || len(db.sessions) != 0 {
		t.Fatal("logout did not revoke local session")
	}
	f.account.AccountID = "usr_22222222222222222222222222222222"
	cookie = login(t, s, f)
	if request(s, "GET", "/api/v1/config", cookie, "").Code != 403 {
		t.Fatal("sign-in promoted an unrelated user")
	}
	if !strings.Contains(request(s, "GET", "/auth/session", cookie, "").Body.String(), `"admin":false`) {
		t.Fatal("member role was not explicit")
	}
}
func TestProxyCredentialBoundaryAndOperatorRecovery(t *testing.T) {
	seen := false
	upstream := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		seen = true
		if r.Header.Get("Authorization") != "Bearer upstream-only" || r.Header.Get("Cookie") != "" || r.Header.Get("X-Agentd-Authorization") != "" {
			t.Error("frontend credentials crossed the upstream boundary")
		}
		w.Header().Set("Set-Cookie", "backend-secret=hidden")
		w.Header().Set("Access-Control-Allow-Origin", "*")
		w.WriteHeader(200)
		_, _ = w.Write([]byte("verified"))
	}))
	defer upstream.Close()
	s, _, _ := fixture(t)
	var err error
	s.daemon, err = proxy(upstream.URL, "upstream-only", "")
	if err != nil {
		t.Fatal(err)
	}
	r := httptest.NewRequest("POST", "https://agent.example/api/v1/run", nil)
	r.Header.Set("Authorization", "Bearer "+s.cfg.OperatorToken)
	r.Header.Set("Cookie", "browser=secret")
	r.Header.Set("X-Agentd-Authorization", "Bearer forged")
	w := httptest.NewRecorder()
	s.ServeHTTP(w, r)
	if !seen || w.Code != 200 || w.Header().Get("Set-Cookie") != "" || w.Header().Get("Access-Control-Allow-Origin") != "" {
		t.Fatal("proxy credential isolation failed")
	}
}
func TestPrivateConfigAndAdministratorPins(t *testing.T) {
	file := t.TempDir() + "/config.json"
	if os.WriteFile(file, []byte("{}"), 0644) != nil {
		t.Fatal("fixture")
	}
	if _, err := LoadConfig(file); err == nil {
		t.Fatal("public-readable configuration accepted")
	}
	s, _, _ := fixture(t)
	if s.cfg.isAdmin("owner@example.com", shared.Proof{}) {
		t.Fatal("email became administrator proof")
	}
}

func TestPostgresFreshSnapshotPersistence(t *testing.T) {
	dsn := os.Getenv("AGENTC_TEST_DATABASE_URL")
	if dsn == "" {
		t.Skip("set a disposable AGENTC_TEST_DATABASE_URL")
	}
	u, parseErr := url.Parse(dsn)
	if parseErr != nil || !strings.HasSuffix(u.Path, "_test") {
		t.Fatal("refusing schema cleanup outside an explicitly named _test database")
	}
	ctx := context.Background()
	db, err := openStore(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer db.pool.Close()
	defer db.pool.Exec(ctx, "DROP SCHEMA agentc_gateway CASCADE")
	a := attempt{"state", "browser", "apple", "verifier", time.Now().Add(time.Minute)}
	if db.AddAttempt(ctx, a) != nil {
		t.Fatal("attempt not stored")
	}
	if _, err = db.TakeAttempt(ctx, a.StateHash, "wrong", a.Provider); err == nil {
		t.Fatal("wrong browser consumed attempt")
	}
	if value, err := db.TakeAttempt(ctx, a.StateHash, a.BrowserHash, a.Provider); err != nil || value != a.Verifier {
		t.Fatal("matching attempt was lost")
	}
	if _, err = db.TakeAttempt(ctx, a.StateHash, a.BrowserHash, a.Provider); err == nil {
		t.Fatal("stored attempt replayed")
	}
	v := session{"token", ownerAccount, 1, time.Now().Add(time.Hour), shared.Proof{Provider: "apple", Namespace: "cn.hubstack.accounts", Subject: "verified-owner"}}
	if db.PutSession(ctx, v) != nil {
		t.Fatal("session not stored")
	}
	other, err := openStore(ctx, dsn)
	if err != nil {
		t.Fatal("snapshot restart failed", err)
	}
	defer other.pool.Close()
	if got, err := other.Session(ctx, v.TokenHash); err != nil || got.AccountID != ownerAccount || got.Proof != v.Proof {
		t.Fatal("session did not survive another process connection")
	}
	if _, err = db.pool.Exec(ctx, "UPDATE agentc_gateway.schema_snapshot SET fingerprint='obsolete'"); err != nil {
		t.Fatal(err)
	}
	if _, err = openStore(ctx, dsn); err == nil {
		t.Fatal("superseded schema silently migrated")
	}
}

func TestAdministratorProofSurvivesProductScopedAccountIDs(t *testing.T) {
	s, f, _ := fixture(t)
	// The same verified owner receives different IDs in different products.
	s.cfg.AdminAccountIDs = nil
	s.cfg.AdminIdentities = []shared.Proof{{Provider: "apple", Namespace: "cn.hubstack.accounts", Subject: "verified-owner"}}
	f.account.AccountID = "usr_33333333333333333333333333333333"
	cookie := login(t, s, f)
	if request(s, "GET", "/api/v1/config", cookie, "").Code != 204 {
		t.Fatal("verified owner was stranded on the member page by a product-specific account ID")
	}
	s.cfg.AdminIdentities[0].Subject = "another-subject"
	if request(s, "GET", "/api/v1/config", cookie, "").Code != 403 {
		t.Fatal("removed administrator proof retained authorization")
	}
}
