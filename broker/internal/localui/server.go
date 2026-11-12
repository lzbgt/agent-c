package localui

import (
	"bytes"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	_ "embed"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"errors"
	"io"
	"mime"
	"net"
	"net/http"
	"net/http/httputil"
	"net/url"
	"os"
	"path"
	"path/filepath"
	"strings"
	"time"
)

//go:embed login.html
var loginHTML []byte

//go:embed login.js
var loginJS []byte

const cookieName = "agentc_local_session"
const sessionLife = 24 * time.Hour

type Server struct {
	cfg        Config
	loginHash  [32]byte
	sessionKey [32]byte
	proxy      http.Handler
	port       string
	webRoot    string
	now        func() time.Time
}

func New(cfg Config) (*Server, error) {
	if err := cfg.Validate(); err != nil {
		return nil, err
	}
	key, err := readPrivateKey(cfg.LoginKeyFile)
	if err != nil {
		return nil, err
	}
	token, err := readPrivateKey(cfg.DaemonTokenFile)
	if err != nil {
		return nil, err
	}
	root, err := filepath.EvalSymlinks(cfg.WebDir)
	if err != nil {
		return nil, errors.New("cannot resolve web-dir")
	}
	_, port, _ := net.SplitHostPort(cfg.Listen)
	s := &Server{cfg: cfg, loginHash: sha256.Sum256([]byte(key)), port: port, webRoot: root, now: time.Now}
	if _, err = rand.Read(s.sessionKey[:]); err != nil {
		return nil, err
	}
	target, _ := url.Parse(cfg.DaemonURL)
	p := httputil.NewSingleHostReverseProxy(target)
	direct := p.Director
	p.Director = func(r *http.Request) {
		direct(r)
		r.Host = target.Host
		for _, h := range []string{"Cookie", "Authorization", "Origin", "X-Agentd-Authorization", "X-OpenRouter-Key", "X-Forwarded-For", "Forwarded"} {
			r.Header.Del(h)
		}
		r.Header.Set("Authorization", "Bearer "+token)
	}
	tr := http.DefaultTransport.(*http.Transport).Clone()
	tr.Proxy = nil
	tr.ResponseHeaderTimeout = time.Duration(cfg.TimeoutMS)*time.Millisecond + time.Minute
	tr.MaxIdleConnsPerHost = 16
	p.Transport = tr
	p.FlushInterval = -1
	p.ErrorHandler = func(w http.ResponseWriter, _ *http.Request, _ error) {
		fail(w, 502, "Local agent runtime is unavailable")
	}
	p.ModifyResponse = func(r *http.Response) error {
		for _, h := range []string{"Set-Cookie", "Access-Control-Allow-Origin", "Access-Control-Allow-Credentials"} {
			r.Header.Del(h)
		}
		r.Header.Set("Cache-Control", "no-store")
		isolateArtifactResponse(r)
		return nil
	}
	s.proxy = p
	return s, nil
}

// Raw artifacts are untrusted data, including when the UI turns their response
// bodies into Blob URLs. A response CSP is not carried into those Blob URLs,
// so active and unknown media types must also lose their executable MIME type.
func isolateArtifactResponse(r *http.Response) {
	if r.Request == nil || r.Request.URL == nil {
		return
	}
	switch r.Request.URL.Path {
	case "/api/v1/file", "/api/v1/blob":
	default:
		return
	}
	r.Header.Set("Content-Security-Policy", "sandbox; default-src 'none'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'")
	r.Header.Set("X-Content-Type-Options", "nosniff")
	mediaType, params, err := mime.ParseMediaType(r.Header.Get("Content-Type"))
	if err == nil {
		switch mediaType {
		case "text/plain",
			"image/png", "image/jpeg", "image/gif", "image/webp", "image/avif", "image/bmp", "image/x-icon", "image/vnd.microsoft.icon",
			"audio/mpeg", "audio/mp4", "audio/aac", "audio/ogg", "audio/webm", "audio/wav", "audio/x-wav", "audio/flac",
			"video/mp4", "video/webm", "video/ogg":
			r.Header.Set("Content-Type", mime.FormatMediaType(mediaType, params))
			return
		}
	}
	r.Header.Set("Content-Type", "application/octet-stream")
	r.Header.Set("Content-Disposition", "attachment")
}

func reply(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}
func fail(w http.ResponseWriter, status int, message string) {
	reply(w, status, map[string]any{"ok": false, "error": message})
}

func (s *Server) hostOK(hostport string) bool {
	host, port, err := net.SplitHostPort(hostport)
	if err != nil || port != s.port {
		return false
	}
	if host == "localhost" {
		return true
	}
	ip := net.ParseIP(host)
	return ip != nil && ip.IsLoopback()
}
func (s *Server) keyOK(v string) bool {
	hash := sha256.Sum256([]byte(v))
	return subtle.ConstantTimeCompare(hash[:], s.loginHash[:]) == 1
}
func (s *Server) sign(payload []byte) []byte {
	m := hmac.New(sha256.New, s.sessionKey[:])
	_, _ = m.Write(payload)
	return m.Sum(nil)
}
func (s *Server) newSession() (string, error) {
	payload := make([]byte, 40)
	binary.BigEndian.PutUint64(payload[:8], uint64(s.now().Add(sessionLife).Unix()))
	if _, err := rand.Read(payload[8:]); err != nil {
		return "", err
	}
	return base64.RawURLEncoding.EncodeToString(append(payload, s.sign(payload)...)), nil
}
func (s *Server) sessionOK(v string) bool {
	raw, err := base64.RawURLEncoding.Strict().DecodeString(v)
	if err != nil || len(raw) != 72 {
		return false
	}
	expiry := binary.BigEndian.Uint64(raw[:8])
	return expiry > uint64(s.now().Unix()) && expiry <= uint64(s.now().Add(sessionLife).Unix()) && hmac.Equal(raw[40:], s.sign(raw[:40]))
}
func (s *Server) authenticated(r *http.Request) (ok, bearer bool) {
	if h := r.Header.Get("Authorization"); h != "" {
		return strings.HasPrefix(h, "Bearer ") && s.keyOK(strings.TrimPrefix(h, "Bearer ")), true
	}
	c, err := r.Cookie(cookieName)
	return err == nil && s.sessionOK(c.Value), false
}
func originOK(r *http.Request, bearer bool) bool {
	origin := r.Header.Get("Origin")
	if origin != "" && origin != "http://"+r.Host {
		return false
	}
	if r.Method != "GET" && r.Method != "HEAD" && !bearer && origin == "" {
		return false
	}
	site := r.Header.Get("Sec-Fetch-Site")
	return site == "" || site == "same-origin" || site == "none"
}
func decodeObject(w http.ResponseWriter, r *http.Request, max int64) (map[string]any, error) {
	ct, _, err := mime.ParseMediaType(r.Header.Get("Content-Type"))
	if err != nil || ct != "application/json" {
		return nil, errors.New("Content-Type must be application/json")
	}
	r.Body = http.MaxBytesReader(w, r.Body, max)
	d := json.NewDecoder(r.Body)
	d.UseNumber()
	var body map[string]any
	if err := d.Decode(&body); err != nil || body == nil {
		return nil, errors.New("invalid JSON object or request too large")
	}
	var extra any
	if d.Decode(&extra) != io.EOF {
		return nil, errors.New("expected one JSON object")
	}
	return body, nil
}
func (s *Server) login(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		fail(w, 405, "Method not allowed")
		return
	}
	if !originOK(r, false) {
		fail(w, 403, "Request origin does not match this local workspace")
		return
	}
	body, err := decodeObject(w, r, 8192)
	if err != nil {
		fail(w, 400, err.Error())
		return
	}
	key, _ := body["key"].(string)
	if !s.keyOK(key) {
		fail(w, 401, "The local access key is incorrect")
		return
	}
	token, err := s.newSession()
	if err != nil {
		fail(w, 500, "Cannot create browser session")
		return
	}
	http.SetCookie(w, &http.Cookie{Name: cookieName, Value: token, Path: "/", HttpOnly: true, SameSite: http.SameSiteStrictMode, MaxAge: int(sessionLife.Seconds())})
	reply(w, 200, map[string]any{"ok": true})
}
func (s *Server) pinRun(w http.ResponseWriter, r *http.Request) bool {
	body, err := decodeObject(w, r, 1<<20)
	if err != nil {
		fail(w, 400, err.Error())
		return false
	}
	// Do not forward browser-supplied credentials or a second token-cap field.
	// An omitted api_key preserves agentd's private local model credential.
	for _, key := range []string{"api_key", "max_completion_tokens", "tools_root"} {
		delete(body, key)
	}
	for key, value := range map[string]any{
		"model": s.cfg.Model, "summary_model": s.cfg.Model, "base_url": s.cfg.ModelBaseURL,
		"proxy": "", "max_tokens": s.cfg.MaxOutputTokens, "max_chars": s.cfg.MaxChars,
		"timeout_ms": s.cfg.TimeoutMS, "tools": "host", "host_policy": "full",
		"automation_profile": "full", "yolo": true, "max_steps": 32,
		"max_tool_calls_total": 64, "max_repeated_tool_calls": 3,
		"max_tool_call_args_chars": 65536,
	} {
		body[key] = value
	}
	raw, err := json.Marshal(body)
	if err != nil {
		fail(w, 400, "Invalid run request")
		return false
	}
	r.Body = io.NopCloser(bytes.NewReader(raw))
	r.ContentLength = int64(len(raw))
	r.Header.Set("Content-Length", stringLength(len(raw)))
	return true
}

func stringLength(n int) string {
	// json.Marshal of an int is a decimal ASCII representation.
	b, _ := json.Marshal(n)
	return string(b)
}

func allowedRoute(method, p string) bool {
	if method == "GET" {
		switch p {
		case "/api/v1/health", "/api/v1/caps", "/api/v1/config", "/api/v1/tools",
			"/api/v1/sessions", "/api/v1/session", "/api/v1/session/work",
			"/api/v1/session/audit", "/api/v1/session/client_events", "/api/v1/session/clients",
			"/api/v1/session/artifacts", "/api/v1/session/scene",
			"/api/v1/job", "/api/v1/job/stream", "/api/v1/trace",
			"/api/v1/db/runs", "/api/v1/db/run", "/api/v1/db/messages", "/api/v1/db/sessions",
			"/api/v1/db/artifacts", "/api/v1/db/ui_actions", "/api/v1/db/client_events",
			"/api/v1/run/replay", "/api/v1/run/attestation", "/api/v1/file",
			"/api/v1/blob", "/api/v1/blob/meta":
			return true
		}
	}
	if method == "POST" {
		switch p {
		case "/api/v1/run", "/api/v1/run_async", "/api/v1/job/cancel",
			"/api/v1/session/new", "/api/v1/session/upload",
			"/api/v1/session/client_event", "/api/v1/session/ui_event":
			return true
		}
	}
	return method == "DELETE" && (p == "/api/v1/session" || p == "/api/v1/job")
}

func (s *Server) configJS(w http.ResponseWriter) {
	cfg := map[string]any{
		"identityEnabled": false, "localModelProfile": true, "connectionMode": "direct",
		"clientId": "qwen35-local-web", "tools": "host", "hostPolicy": "full",
		"yolo": true, "automationProfile": "full", "model": s.cfg.Model,
		"baseUrl": s.cfg.ModelBaseURL, "apiKey": "", "proxyUrl": "",
		"timeoutMs": s.cfg.TimeoutMS, "streamAssistant": true, "useAsync": true,
		"trace": false, "verbose": false, "serverPrefsMode": "off",
		"allowClientRpcs": false, "allowClientEffects": false, "allowUnsafePageEval": false,
	}
	raw, _ := json.Marshal(cfg)
	w.Header().Set("Content-Type", "application/javascript; charset=utf-8")
	_, _ = w.Write(append(append([]byte("window.__AGENT_UI_CONFIG__="), raw...), []byte(";window.__AGENT_UI_CONFIG__.daemonBaseUrl=window.location.origin;\n")...))
}

func (s *Server) static(w http.ResponseWriter, r *http.Request) {
	p := path.Clean(r.URL.Path)
	if p == "/" {
		p = "/index.html"
	}
	if strings.Contains(p, "/.") || (!strings.HasPrefix(p, "/assets/") && p != "/index.html" && p != "/favicon.ico") {
		fail(w, 404, "Not found")
		return
	}
	name, err := filepath.EvalSymlinks(filepath.Join(s.webRoot, p))
	if err != nil {
		fail(w, 404, "Not found")
		return
	}
	rel, err := filepath.Rel(s.webRoot, name)
	if err != nil || rel == ".." || strings.HasPrefix(rel, ".."+string(filepath.Separator)) {
		fail(w, 404, "Not found")
		return
	}
	st, err := os.Stat(name)
	if err != nil || !st.Mode().IsRegular() {
		fail(w, 404, "Not found")
		return
	}
	http.ServeFile(w, r, name)
}

func (s *Server) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.Header().Set("X-Frame-Options", "DENY")
	w.Header().Set("Referrer-Policy", "no-referrer")
	w.Header().Set("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data: https:; connect-src 'self'; frame-src 'none'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'; form-action 'self'")
	if !s.hostOK(r.Host) {
		fail(w, 403, "This server accepts only its loopback host and port")
		return
	}
	if r.URL.RawPath != "" || strings.Contains(r.URL.Path, "\\") || path.Clean(r.URL.Path) != r.URL.Path {
		fail(w, 400, "Invalid request path")
		return
	}
	if r.URL.Path == "/healthz" && r.Method == "GET" {
		reply(w, 200, map[string]any{"ok": true, "service": "agent-c-local-ui", "mode": "local-model"})
		return
	}
	if r.URL.Path == "/local/login" {
		s.login(w, r)
		return
	}
	if r.URL.Path == "/local-login.js" && r.Method == "GET" {
		w.Header().Set("Content-Type", "application/javascript; charset=utf-8")
		_, _ = w.Write(loginJS)
		return
	}
	ok, bearer := s.authenticated(r)
	if !ok {
		if (r.URL.Path == "/" || r.URL.Path == "/index.html") && r.Method == "GET" {
			w.Header().Set("Content-Type", "text/html; charset=utf-8")
			_, _ = w.Write(loginHTML)
			return
		}
		fail(w, 401, "Unlock this local workspace with its access key")
		return
	}
	if !originOK(r, bearer) {
		fail(w, 403, "Request origin does not match this local workspace")
		return
	}
	if r.URL.Path == "/local/logout" && r.Method == "POST" {
		http.SetCookie(w, &http.Cookie{Name: cookieName, Value: "", Path: "/", HttpOnly: true, SameSite: http.SameSiteStrictMode, MaxAge: -1})
		reply(w, 200, map[string]any{"ok": true})
		return
	}
	if strings.HasPrefix(r.URL.Path, "/api/") {
		if !allowedRoute(r.Method, r.URL.Path) {
			fail(w, 403, "This operation is unavailable in the local model profile")
			return
		}
		if r.URL.Path == "/api/v1/run" || r.URL.Path == "/api/v1/run_async" {
			if !s.pinRun(w, r) {
				return
			}
		} else {
			r.Body = http.MaxBytesReader(w, r.Body, 32<<20)
		}
		s.proxy.ServeHTTP(w, r)
		return
	}
	if r.Method != "GET" && r.Method != "HEAD" {
		fail(w, 405, "Method not allowed")
		return
	}
	if r.URL.Path == "/agentui-config.js" {
		s.configJS(w)
		return
	}
	s.static(w, r)
}
