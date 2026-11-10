package gateway

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"crypto/tls"
	"crypto/x509"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	shared "github.com/lzbgt/hubstack-payments/sdk/identity"
	"io/fs"
	"log"
	"net/http"
	"net/http/httputil"
	"net/url"
	"os"
	"path"
	"strings"
	"time"
)

const sessionCookie = "__Host-agentc_session"
const attemptCookie = "__Host-agentc_oauth"

type identity interface {
	Providers(context.Context) (map[string]bool, error)
	Begin(context.Context, string, string, string, string) (shared.Start, error)
	Exchange(context.Context, string, string, string, string, string) (shared.Result, error)
	AccountState(context.Context, string) (shared.AccountState, error)
	Logout(context.Context, string) error
}
type Server struct {
	voice          *voiceProxy
	cfg            Config
	db             store
	identity       identity
	daemon, broker http.Handler
	files          http.Handler
	close          func()
	oauthSlots     chan struct{}
}

func New(ctx context.Context, cfg Config) (*Server, error) {
	if err := cfg.Validate(); err != nil {
		return nil, err
	}
	client, err := shared.New("https://payments.hubstack.cn", cfg.IdentityCredential)
	if err != nil {
		return nil, err
	}
	db, err := openStore(ctx, cfg.DatabaseURL)
	if err != nil {
		return nil, err
	}
	s := &Server{cfg: cfg, db: db, identity: client, close: db.pool.Close, oauthSlots: make(chan struct{}, 4)}
	s.voice, err = newVoiceProxy(cfg.Voice)
	if err != nil {
		log.Print("Voice unavailable: ", err)
	}
	s.daemon, err = proxy(cfg.DaemonURL, cfg.DaemonToken, "")
	if err != nil {
		s.Close()
		return nil, err
	}
	if cfg.BrokerURL != "" {
		s.broker, err = proxy(cfg.BrokerURL, cfg.BrokerToken, cfg.BrokerCAFile)
		if err != nil {
			s.Close()
			return nil, err
		}
	}
	if st, err := os.Stat(cfg.WebDir + "/index.html"); err != nil || !st.Mode().IsRegular() {
		s.Close()
		return nil, errors.New("gateway web_dir must contain a built index.html")
	}
	s.files = http.FileServer(http.Dir(cfg.WebDir))
	return s, nil
}
func (s *Server) Close() {
	if s.voice != nil {
		s.voice.close()
	}
	if s.close != nil {
		s.close()
	}
}
func jsonResponse(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}
func fail(w http.ResponseWriter, status int, message string) {
	jsonResponse(w, status, map[string]any{"ok": false, "error": message})
}
func digest(value string) string {
	sum := sha256.Sum256([]byte(value))
	return hex.EncodeToString(sum[:])
}
func randomToken() (string, error) {
	b := make([]byte, 32)
	if _, err := rand.Read(b); err != nil {
		return "", err
	}
	return base64.RawURLEncoding.EncodeToString(b), nil
}
func canonicalToken(value string) bool {
	b, err := base64.RawURLEncoding.Strict().DecodeString(value)
	return err == nil && len(b) == 32 && len(value) == 43
}
func providerNamespace(provider string) string {
	return map[string]string{"google": "accounts.google.com", "github": "github.com", "apple": "cn.hubstack.accounts"}[provider]
}
func setCookie(w http.ResponseWriter, name, value string, age int) {
	http.SetCookie(w, &http.Cookie{Name: name, Value: value, Path: "/", Secure: true, HttpOnly: true, SameSite: http.SameSiteLaxMode, MaxAge: age})
}

func proxy(raw, token, caFile string) (http.Handler, error) {
	target, err := url.Parse(raw)
	if err != nil {
		return nil, err
	}
	p := httputil.NewSingleHostReverseProxy(target)
	original := p.Director
	p.Director = func(r *http.Request) {
		original(r)
		r.Host = target.Host
		r.Header.Del("Cookie")
		r.Header.Del("X-Agentd-Authorization")
		r.Header.Del("X-Forwarded-For")
		r.Header.Set("Authorization", "Bearer "+token)
	}
	t := http.DefaultTransport.(*http.Transport).Clone()
	t.MaxIdleConnsPerHost = 16
	t.ResponseHeaderTimeout = 30 * time.Second
	if caFile != "" {
		b, e := os.ReadFile(caFile)
		if e != nil {
			return nil, errors.New("cannot read upstream CA")
		}
		pool := x509.NewCertPool()
		if !pool.AppendCertsFromPEM(b) {
			return nil, errors.New("invalid upstream CA")
		}
		t.TLSClientConfig = &tls.Config{RootCAs: pool, MinVersion: tls.VersionTLS12}
	}
	p.Transport = t
	p.FlushInterval = -1
	p.ErrorHandler = func(w http.ResponseWriter, r *http.Request, err error) {
		fail(w, 502, "Execution service is unavailable")
	}
	p.ModifyResponse = func(r *http.Response) error {
		r.Header.Del("Set-Cookie")
		r.Header.Del("Access-Control-Allow-Origin")
		r.Header.Set("Cache-Control", "no-store")
		return nil
	}
	return p, nil
}

func (s *Server) authenticate(r *http.Request) (string, bool, int) {
	const prefix = "Bearer "
	if raw := r.Header.Get("Authorization"); strings.HasPrefix(raw, prefix) && subtle.ConstantTimeCompare([]byte(strings.TrimPrefix(raw, prefix)), []byte(s.cfg.OperatorToken)) == 1 {
		return "operator", true, 200
	}
	c, err := r.Cookie(sessionCookie)
	if err != nil || !canonicalToken(c.Value) {
		return "", false, 401
	}
	v, err := s.db.Session(r.Context(), digest(c.Value))
	if err != nil {
		return "", false, 401
	}
	state, err := s.identity.AccountState(r.Context(), v.AccountID)
	if err != nil {
		return "", false, 503
	}
	if state.ApplicationID != s.cfg.ApplicationID || state.InstanceID != s.cfg.InstanceID || state.AccountID != v.AccountID || !state.Active || state.Revision != v.Revision {
		return "", false, 401
	}
	return v.AccountID, s.cfg.isAdmin(v.AccountID, v.Proof), 200
}
func (s *Server) originOK(r *http.Request, operator bool) bool {
	origin := r.Header.Get("Origin")
	if origin != "" && origin != s.cfg.PublicURL {
		return false
	}
	return r.Method == "GET" || r.Method == "HEAD" || operator || origin == s.cfg.PublicURL
}
func (s *Server) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.Header().Set("X-Frame-Options", "DENY")
	w.Header().Set("Referrer-Policy", "same-origin")
	if r.URL.Path == "/healthz" {
		ctx, cancel := context.WithTimeout(r.Context(), 2*time.Second)
		defer cancel()
		if s.db.Ping(ctx) != nil {
			fail(w, 503, "Storage is unavailable")
			return
		}
		jsonResponse(w, 200, map[string]any{"ok": true, "service": "agent-c-gateway", "source_revision": s.cfg.SourceRevision})
		return
	}
	if r.URL.Path == "/auth/providers" && r.Method == "GET" {
		providers, err := s.identity.Providers(r.Context())
		if err != nil {
			fail(w, 503, "Sign-in is temporarily unavailable")
			return
		}
		jsonResponse(w, 200, providers)
		return
	}
	if strings.HasPrefix(r.URL.Path, "/auth/") {
		s.authRoute(w, r)
		return
	}
	if strings.HasPrefix(r.URL.Path, "/api/") || strings.HasPrefix(r.URL.Path, "/broker/") || strings.HasPrefix(r.URL.Path, "/voice/") {
		account, admin, status := s.authenticate(r)
		if status != 200 {
			fail(w, status, "Sign in to continue")
			return
		}
		if !admin {
			fail(w, 403, "Administrator access is required")
			return
		}
		if !s.originOK(r, account == "operator") {
			fail(w, 403, "Request origin does not match this workspace")
			return
		}
		if strings.HasPrefix(r.URL.Path, "/voice/") {
			s.voiceRoute(w, r, account)
			return
		}
		if strings.HasPrefix(r.URL.Path, "/broker/") {
			if s.broker == nil {
				fail(w, 404, "Broker is not configured")
				return
			}
			http.StripPrefix("/broker", s.broker).ServeHTTP(w, r)
		} else {
			s.daemon.ServeHTTP(w, r)
		}
		return
	}
	if r.Method != "GET" && r.Method != "HEAD" {
		fail(w, 405, "Method not allowed")
		return
	}
	if r.URL.Path == "/agentui-config.js" {
		w.Header().Set("Content-Type", "application/javascript")
		w.Header().Set("Cache-Control", "no-store")
		_, _ = w.Write([]byte(`window.__AGENT_UI_CONFIG__={identityEnabled:true,connectionMode:"direct",daemonBaseUrl:window.location.origin,brokerBaseUrl:window.location.origin+"/broker",brokerAgentId:"arch",brokerCookieAuth:true,tools:"host",hostPolicy:"full",yolo:true,model:"deepseek-v4-pro",proxyUrl:"",apiKey:"",serverPrefsMode:"off"};`))
		return
	}
	// Keep dotfiles and source paths out of the static release surface.
	clean := path.Clean(r.URL.Path)
	if strings.Contains(clean, "/.") || !fs.ValidPath(strings.TrimPrefix(clean, "/")) && clean != "/" {
		fail(w, 404, "Not found")
		return
	}
	if _, err := os.Stat(s.cfg.WebDir + clean); err != nil {
		cloned := r.Clone(r.Context())
		u := *r.URL
		cloned.URL = &u
		cloned.URL.Path = "/"
		s.files.ServeHTTP(w, cloned)
		return
	}
	s.files.ServeHTTP(w, r)
}
