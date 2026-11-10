package gateway

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/url"
	"os"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/gorilla/websocket"
)

const defaultVoiceModel = "qwen3.5-omni-flash-realtime"
const voiceMaximumSession = 5 * time.Minute

type VoiceConfig struct {
	EnvFile  string `json:"env_file"`
	Endpoint string `json:"endpoint"`
	Model    string `json:"model"`
	Voice    string `json:"voice"`
}

func (c VoiceConfig) defaults() VoiceConfig {
	if c.Endpoint == "" {
		c.Endpoint = "wss://dashscope.aliyuncs.com/api-ws/v1/realtime"
	}
	if c.Model == "" {
		c.Model = defaultVoiceModel
	}
	if c.Voice == "" {
		c.Voice = "Ethan"
	}
	return c
}
func (c VoiceConfig) validate() error {
	u, err := url.Parse(c.Endpoint)
	if err != nil || u.Scheme != "wss" || u.User != nil || u.Port() != "" || u.Path != "/api-ws/v1/realtime" || u.RawQuery != "" || u.Fragment != "" {
		return errors.New("voice requires an exact Aliyun realtime endpoint")
	}
	classic := u.Host == "dashscope.aliyuncs.com" || u.Host == "dashscope-intl.aliyuncs.com"
	if !classic && !strings.HasSuffix(u.Host, ".maas.aliyuncs.com") {
		return errors.New("voice endpoint must belong to Aliyun Model Studio")
	}
	switch c.Model {
	case defaultVoiceModel, "qwen3.5-omni-plus-realtime":
	case "qwen3.8-omni-flash-realtime":
		if classic {
			return errors.New("this voice model requires a workspace endpoint")
		}
	default:
		return errors.New("unsupported Aliyun realtime voice model")
	}
	if c.Voice != "Ethan" && c.Voice != "Tina" && c.Voice != "Cherry" {
		return errors.New("unsupported voice")
	}
	return nil
}
func loadVoiceKey(path string) (string, error) {
	f, err := os.OpenFile(path, os.O_RDONLY|syscall.O_NOFOLLOW|syscall.O_NONBLOCK, 0)
	if err != nil {
		return "", errors.New("voice credential file unavailable")
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil || !st.Mode().IsRegular() || st.Mode().Perm()&0077 != 0 || st.Size() > 1<<20 {
		return "", errors.New("voice credential file must be owner-only and bounded")
	}
	scan := bufio.NewScanner(io.LimitReader(f, 1<<20))
	scan.Buffer(make([]byte, 4096), 64<<10)
	var key string
	for scan.Scan() {
		line := strings.TrimSpace(strings.TrimPrefix(strings.TrimSpace(scan.Text()), "export "))
		name, value, ok := strings.Cut(line, "=")
		if ok && (strings.TrimSpace(name) == "ALIYUN_MODEL_STUDIO_AK" || strings.TrimSpace(name) == "DASHSCOPE_API_KEY") {
			value = strings.Trim(strings.TrimSpace(value), "\"'")
			if strings.TrimSpace(name) == "ALIYUN_MODEL_STUDIO_AK" || key == "" {
				key = value
			}
		}
	}
	if scan.Err() != nil || len(key) < 16 || len(key) > 1024 || strings.ContainsAny(key, " \r\n\t") {
		return "", errors.New("Model Studio voice credential unavailable")
	}
	return key, nil
}

type voiceProxy struct {
	cfg          VoiceConfig
	key          string
	dialer       *websocket.Dialer
	mu           sync.Mutex
	active       map[string]context.CancelFunc
	closed       bool
	maxSession   time.Duration
	authInterval time.Duration
}

func newVoiceProxy(cfg VoiceConfig) (*voiceProxy, error) {
	if cfg.EnvFile == "" {
		return nil, nil
	}
	cfg = cfg.defaults()
	if err := cfg.validate(); err != nil {
		return nil, err
	}
	key, err := loadVoiceKey(cfg.EnvFile)
	if err != nil {
		return nil, err
	}
	return &voiceProxy{cfg: cfg, key: key, dialer: &websocket.Dialer{HandshakeTimeout: 12 * time.Second, Proxy: http.ProxyFromEnvironment}, active: map[string]context.CancelFunc{}, maxSession: voiceMaximumSession, authInterval: 15 * time.Second}, nil
}
func (p *voiceProxy) close() {
	p.mu.Lock()
	p.closed = true
	for _, cancel := range p.active {
		cancel()
	}
	p.mu.Unlock()
}
func (p *voiceProxy) reserve(account string, cancel context.CancelFunc) bool {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.closed || len(p.active) >= 4 || p.active[account] != nil {
		return false
	}
	p.active[account] = cancel
	return true
}
func (p *voiceProxy) release(account string) { p.mu.Lock(); delete(p.active, account); p.mu.Unlock() }
func (s *Server) voiceRoute(w http.ResponseWriter, r *http.Request, account string) {
	if r.Method != "GET" {
		fail(w, 405, "Method not allowed")
		return
	}
	if r.URL.Path == "/voice/v1/config" {
		cfg := s.cfg.Voice.defaults()
		jsonResponse(w, 200, map[string]any{"ok": true, "available": s.voice != nil, "protocol": "agentc.voice.v1", "provider": "aliyun", "model": cfg.Model, "voice": cfg.Voice, "input_sample_rate": 16000, "output_sample_rate": 24000, "image_format": "jpeg", "image_max_base64_bytes": 256 << 10, "max_session_seconds": int(voiceMaximumSession.Seconds())})
		return
	}
	if r.URL.Path != "/voice/v1/realtime" {
		fail(w, 404, "Voice endpoint not found")
		return
	}
	if s.voice == nil {
		fail(w, 503, "Voice is unavailable.")
		return
	}
	if r.Header.Get("Origin") != s.cfg.PublicURL && account != "operator" {
		fail(w, 403, "Request origin does not match this workspace")
		return
	}
	s.voice.serve(w, r, account, func(ctx context.Context) bool {
		a, admin, status := s.authenticate(r.WithContext(ctx))
		return status == 200 && admin && a == account
	})
}
func (p *voiceProxy) serve(w http.ResponseWriter, r *http.Request, account string, authorized func(context.Context) bool) {
	ctx, cancel := context.WithTimeout(r.Context(), p.maxSession)
	defer cancel()
	if !p.reserve(account, cancel) {
		fail(w, 409, "A voice call is already active or voice capacity is full.")
		return
	}
	defer p.release(account)
	endpoint, _ := url.Parse(p.cfg.Endpoint)
	q := endpoint.Query()
	q.Set("model", p.cfg.Model)
	endpoint.RawQuery = q.Encode()
	upstream, response, err := p.dialer.DialContext(ctx, endpoint.String(), http.Header{"Authorization": []string{"Bearer " + p.key}})
	if err != nil && response != nil && response.Body != nil {
		response.Body.Close()
	}
	if err != nil {
		fail(w, 502, "Voice connection is unavailable.")
		return
	}
	defer upstream.Close()
	upgrade := websocket.Upgrader{HandshakeTimeout: 10 * time.Second, CheckOrigin: func(*http.Request) bool { return true }}
	client, err := upgrade.Upgrade(w, r, nil)
	if err != nil {
		return
	}
	defer client.Close()
	client.SetReadLimit(384 << 10)
	upstream.SetReadLimit(1 << 20)
	deadline := time.Now().Add(p.maxSession)
	client.SetReadDeadline(deadline)
	upstream.SetReadDeadline(deadline)
	var ready atomic.Bool
	var upstreamMu sync.Mutex
	writeUp := func(data []byte) error {
		upstreamMu.Lock()
		defer upstreamMu.Unlock()
		upstream.SetWriteDeadline(time.Now().Add(10 * time.Second))
		return upstream.WriteMessage(websocket.TextMessage, data)
	}
	done := make(chan struct{})
	go func() {
		defer close(done)
		defer cancel()
		for {
			kind, data, err := upstream.ReadMessage()
			if err != nil {
				return
			}
			if kind != websocket.TextMessage || !json.Valid(data) {
				return
			}
			var header struct {
				Type string `json:"type"`
			}
			if json.Unmarshal(data, &header) != nil {
				return
			}
			if header.Type == "session.created" {
				config, _ := json.Marshal(map[string]any{"event_id": "agentc_voice_config", "type": "session.update", "session": map[string]any{"modalities": []string{"text", "audio"}, "voice": p.cfg.Voice, "input_audio_format": "pcm", "output_audio_format": "pcm", "input_audio_transcription": map[string]string{"model": "qwen3-asr-flash-realtime"}, "turn_detection": map[string]any{"type": "server_vad", "threshold": 0.5, "silence_duration_ms": 800}, "max_tokens": 2048, "instructions": "You are the voice assistant in Agent-C. Be helpful, concise and natural. Understand the user's speech and any shared images. Reply in the user's language. Never claim to have used workspace tools or changed anything unless execution evidence establishes it."}})
				if writeUp(config) != nil {
					return
				}
			}
			if header.Type == "session.updated" {
				ready.Store(true)
			}
			data = bytes.ReplaceAll(data, []byte(p.key), []byte("[redacted]"))
			client.SetWriteDeadline(time.Now().Add(10 * time.Second))
			if client.WriteMessage(websocket.TextMessage, data) != nil {
				return
			}
		}
	}()
	watcherDone := make(chan struct{})
	go func() {
		defer close(watcherDone)
		tick := time.NewTicker(p.authInterval)
		defer tick.Stop()
		for {
			select {
			case <-ctx.Done():
				client.Close()
				upstream.Close()
				return
			case <-tick.C:
				check, finish := context.WithTimeout(ctx, 5*time.Second)
				allowed := authorized(check)
				finish()
				if !allowed {
					cancel()
					continue
				}
				if client.WriteControl(websocket.PingMessage, nil, time.Now().Add(5*time.Second)) != nil {
					cancel()
				}
			}
		}
	}()
	limit := newVoiceInput()
	for {
		kind, data, err := client.ReadMessage()
		if err != nil {
			break
		}
		if kind != websocket.TextMessage || !ready.Load() {
			break
		}
		if err = limit.validate(data, time.Now()); err != nil {
			_ = client.WriteControl(websocket.CloseMessage, websocket.FormatCloseMessage(websocket.ClosePolicyViolation, "Invalid voice input"), time.Now().Add(time.Second))
			break
		}
		if writeUp(data) != nil {
			break
		}
	}
	cancel()
	client.Close()
	upstream.Close()
	<-done
	<-watcherDone
}
