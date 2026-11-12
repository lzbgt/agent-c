package localui

import (
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func fixture(t *testing.T, handler http.HandlerFunc) (*Server, string) {
	t.Helper()
	up := httptest.NewServer(handler)
	t.Cleanup(up.Close)
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "index.html"), []byte("<html>existing UI</html>"), 0600); err != nil {
		t.Fatal(err)
	}
	key := "test-local-login-key-0123456789"
	if err := os.WriteFile(filepath.Join(dir, "login-key"), []byte(key), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "daemon-key"), []byte("test-private-daemon-key-0123456789"), 0600); err != nil {
		t.Fatal(err)
	}
	cfg := Config{Listen: "127.0.0.1:18088", DaemonURL: up.URL, DaemonTokenFile: filepath.Join(dir, "daemon-key"), LoginKeyFile: filepath.Join(dir, "login-key"), WebDir: dir, Model: "local-test", ModelBaseURL: "http://127.0.0.1:18089/v1", MaxOutputTokens: 8192, MaxChars: 480000, TimeoutMS: 600000}
	s, err := New(cfg)
	if err != nil {
		t.Fatal(err)
	}
	return s, key
}

func call(s *Server, method, route, body, origin string, cookie *http.Cookie) *httptest.ResponseRecorder {
	r := httptest.NewRequest(method, "http://127.0.0.1:18088"+route, strings.NewReader(body))
	if body != "" {
		r.Header.Set("Content-Type", "application/json")
	}
	if origin != "" {
		r.Header.Set("Origin", origin)
	}
	if cookie != nil {
		r.AddCookie(cookie)
	}
	w := httptest.NewRecorder()
	s.ServeHTTP(w, r)
	return w
}
func login(t *testing.T, s *Server, key string) *http.Cookie {
	t.Helper()
	b, _ := json.Marshal(map[string]string{"key": key})
	w := call(s, "POST", "/local/login", string(b), "http://127.0.0.1:18088", nil)
	if w.Code != 200 {
		t.Fatalf("login failed: %d %s", w.Code, w.Body.String())
	}
	cs := w.Result().Cookies()
	if len(cs) != 1 {
		t.Fatal("missing session cookie")
	}
	return cs[0]
}

func TestLocalAuthAndOrigin(t *testing.T) {
	s, key := fixture(t, func(w http.ResponseWriter, r *http.Request) { w.WriteHeader(200) })
	if w := call(s, "GET", "/api/v1/sessions", "", "", nil); w.Code != 401 {
		t.Fatal("unauthenticated API allowed")
	}
	if w := call(s, "GET", "/", "", "", nil); w.Code != 200 || !strings.Contains(w.Body.String(), "Unlock your local agent") {
		t.Fatal("login page missing")
	}
	if w := call(s, "POST", "/local/login", `{"key":"wrong"}`, "http://127.0.0.1:18088", nil); w.Code != 401 {
		t.Fatal("wrong key allowed")
	}
	if w := call(s, "POST", "/local/login", `{"key":"wrong"}`, "https://foreign.example", nil); w.Code != 403 {
		t.Fatal("foreign origin allowed")
	}
	if w := call(s, "POST", "/local/login", `{"key":"wrong"}`, "", nil); w.Code != 403 {
		t.Fatal("missing login origin allowed")
	}
	c := login(t, s, key)
	if !c.HttpOnly || c.SameSite != http.SameSiteStrictMode || c.Value == key {
		t.Fatal("unsafe browser cookie")
	}
	if w := call(s, "GET", "/api/v1/sessions", "", "", c); w.Code != 200 {
		t.Fatal("valid browser cookie rejected")
	}
	if w := call(s, "POST", "/api/v1/session/new", `{}`, "https://foreign.example", c); w.Code != 403 {
		t.Fatal("cross-origin mutation allowed")
	}
	if w := call(s, "POST", "/api/v1/session/new", `{}`, "", c); w.Code != 403 {
		t.Fatal("originless cookie mutation allowed")
	}
	r := httptest.NewRequest("GET", "http://rebinding.example:18088/api/v1/sessions", nil)
	r.AddCookie(c)
	w := httptest.NewRecorder()
	s.ServeHTTP(w, r)
	if w.Code != 403 {
		t.Fatal("DNS rebinding host allowed")
	}
	r = httptest.NewRequest("GET", "http://127.0.0.1:18088/api/v1/sessions", nil)
	r.AddCookie(c)
	r.Header.Set("Sec-Fetch-Site", "same-site")
	w = httptest.NewRecorder()
	s.ServeHTTP(w, r)
	if w.Code != 403 {
		t.Fatal("other local origin allowed")
	}
	fixed := s.now()
	s.now = func() time.Time { return fixed.Add(25 * time.Hour) }
	if w := call(s, "GET", "/api/v1/sessions", "", "", c); w.Code != 401 {
		t.Fatal("expired browser session allowed")
	}
}

func TestModelPinningAndPrivateUpstream(t *testing.T) {
	var got map[string]any
	s, key := fixture(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Cookie") != "" || r.Header.Get("X-OpenRouter-Key") != "" {
			t.Error("browser credentials forwarded")
		}
		if r.Header.Get("Authorization") != "Bearer test-private-daemon-key-0123456789" {
			t.Error("upstream credential missing")
		}
		if err := json.NewDecoder(r.Body).Decode(&got); err != nil {
			t.Error(err)
		}
		w.Header().Set("Set-Cookie", "upstream=secret")
		w.Header().Set("Access-Control-Allow-Origin", "*")
		_, _ = w.Write([]byte(`{"ok":true,"job_id":"test"}`))
	})
	c := login(t, s, key)
	body := `{"prompt":"Inspect this host","model":"cloud-model","summary_model":"cloud-summary","base_url":"https://cloud.invalid/v1","proxy":"http://other.invalid","api_key":"browser-secret","max_tokens":999999,"max_completion_tokens":999999,"max_chars":1,"tools":"none","tools_root":"/tmp"}`
	r := httptest.NewRequest("POST", "http://127.0.0.1:18088/api/v1/run_async", strings.NewReader(body))
	r.Header.Set("Content-Type", "application/json")
	r.Header.Set("Origin", "http://127.0.0.1:18088")
	r.Header.Set("X-OpenRouter-Key", "ignored")
	r.AddCookie(c)
	w := httptest.NewRecorder()
	s.ServeHTTP(w, r)
	if w.Code != 200 {
		t.Fatalf("run failed: %d %s", w.Code, w.Body.String())
	}
	if got["model"] != "local-test" || got["summary_model"] != "local-test" || got["base_url"] != "http://127.0.0.1:18089/v1" || got["proxy"] != "" || got["tools"] != "host" || got["host_policy"] != "full" {
		t.Fatalf("pinning failed: %#v", got)
	}
	for _, k := range []string{"api_key", "max_completion_tokens", "tools_root"} {
		if _, ok := got[k]; ok {
			t.Fatalf("unsafe field retained: %s", k)
		}
	}
	if got["max_tokens"] != float64(8192) || got["max_chars"] != float64(480000) || got["max_steps"] != float64(32) {
		t.Fatal("local limits missing")
	}
	if w.Header().Get("Set-Cookie") != "" || w.Header().Get("Access-Control-Allow-Origin") != "" {
		t.Fatal("upstream browser headers leaked")
	}
	cfg := call(s, "GET", "/agentui-config.js", "", "", c).Body.String()
	if strings.Contains(cfg, key) || strings.Contains(cfg, "test-private-daemon-key") || !strings.Contains(cfg, `"localModelProfile":true`) {
		t.Fatal("runtime config leaked secret or omitted local mode")
	}
}

func TestBoundedRouteSurface(t *testing.T) {
	calls := 0
	s, key := fixture(t, func(w http.ResponseWriter, r *http.Request) { calls++; w.WriteHeader(200) })
	c := login(t, s, key)
	for _, route := range []string{"/api/v1/config/update", "/api/v1/diagnostics/provider_test", "/api/v1/workflow/submit", "/api/v1/orchestrate", "/api/v1/edge/message"} {
		if w := call(s, "POST", route, `{}`, "http://127.0.0.1:18088", c); w.Code != 403 {
			t.Fatalf("unsafe local-profile route allowed: %s", route)
		}
	}
	if w := call(s, "POST", "/api/v1/run", `{} {}`, "http://127.0.0.1:18088", c); w.Code != 400 {
		t.Fatal("trailing JSON accepted")
	}
	if w := call(s, "POST", "/api/v1/run", `{"prompt":"`+strings.Repeat("x", 1<<20)+`"}`, "http://127.0.0.1:18088", c); w.Code != 400 {
		t.Fatal("oversized run accepted")
	}
	if calls != 0 {
		t.Fatal("blocked requests reached agentd")
	}
	outside := filepath.Join(t.TempDir(), "private")
	_ = os.WriteFile(outside, []byte("private-data"), 0600)
	_ = os.Mkdir(filepath.Join(s.cfg.WebDir, "assets"), 0700)
	_ = os.Symlink(outside, filepath.Join(s.cfg.WebDir, "assets", "private.txt"))
	for _, route := range []string{"/.env", "/assets/private.txt", "/assets/../login-key", "/missing-source.go"} {
		if w := call(s, "GET", route, "", "", c); w.Code < 400 {
			t.Fatalf("unsafe static path served: %s", route)
		}
	}
	if w := call(s, "GET", "/", "", "", c); w.Code != 200 || !strings.Contains(w.Body.String(), "existing UI") {
		t.Fatal("built UI not served")
	}
}

func TestLoopbackConfigurationAndKeyPermissions(t *testing.T) {
	s, _ := fixture(t, func(w http.ResponseWriter, r *http.Request) {})
	cfg := s.cfg
	cfg.Listen = "0.0.0.0:18088"
	if cfg.Validate() == nil {
		t.Fatal("public listener allowed")
	}
	cfg = s.cfg
	cfg.ModelBaseURL = "https://cloud.example/v1"
	if cfg.Validate() == nil {
		t.Fatal("cloud model URL allowed")
	}
	cfg = s.cfg
	cfg.DaemonURL = "http://localhost:18090"
	if cfg.Validate() == nil {
		t.Fatal("DNS upstream allowed")
	}
	if err := os.Chmod(s.cfg.LoginKeyFile, 0644); err != nil {
		t.Fatal(err)
	}
	if _, err := New(s.cfg); err == nil {
		t.Fatal("world-readable login key accepted")
	}
}

func TestProxyStreamsAndCancellation(t *testing.T) {
	released := make(chan struct{})
	s, key := fixture(t, func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/event-stream")
		_, _ = io.WriteString(w, "data: first\n\n")
		w.(http.Flusher).Flush()
		<-r.Context().Done()
		close(released)
	})
	server := httptest.NewServer(s)
	defer server.Close()
	// The production server validates its configured Host, independent of the test listener.
	req, _ := http.NewRequest("GET", server.URL+"/api/v1/job/stream?job_id=test", nil)
	req.Host = "127.0.0.1:18088"
	req.Header.Set("Authorization", "Bearer "+key)
	client := &http.Client{Timeout: 3 * time.Second}
	res, err := client.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	b := make([]byte, len("data: first\n\n"))
	if _, err = io.ReadFull(res.Body, b); err != nil {
		t.Fatal(err)
	}
	if string(b) != "data: first\n\n" {
		t.Fatal("SSE data not streamed")
	}
	_ = res.Body.Close()
	select {
	case <-released:
	case <-time.After(2 * time.Second):
		t.Fatal("upstream did not see cancellation")
	}
}
