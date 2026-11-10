package gateway

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"github.com/gorilla/websocket"
	"image"
	"image/color"
	"image/jpeg"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func voiceJPEG(t *testing.T) string {
	t.Helper()
	var b bytes.Buffer
	img := image.NewRGBA(image.Rect(0, 0, 16, 16))
	img.Set(1, 1, color.White)
	if jpeg.Encode(&b, img, nil) != nil {
		t.Fatal("jpeg encode")
	}
	return base64.StdEncoding.EncodeToString(b.Bytes())
}
func voiceEvent(kind string, fields map[string]any) []byte {
	if fields == nil {
		fields = map[string]any{}
	}
	fields["type"] = kind
	fields["event_id"] = "test-event"
	b, _ := json.Marshal(fields)
	return b
}
func TestVoiceConfigurationAndPrivateKey(t *testing.T) {
	c := VoiceConfig{}.defaults()
	if c.Model != defaultVoiceModel || c.validate() != nil {
		t.Fatal("default voice invalid")
	}
	for _, bad := range []string{"ws://dashscope.aliyuncs.com/api-ws/v1/realtime", "wss://evil.example/api-ws/v1/realtime", "wss://dashscope.aliyuncs.com/api-ws/v1/realtime?key=secret", "wss://user:pass@dashscope.aliyuncs.com/api-ws/v1/realtime"} {
		cfg := c
		cfg.Endpoint = bad
		if cfg.validate() == nil {
			t.Fatal("unsafe endpoint accepted")
		}
	}
	file := filepath.Join(t.TempDir(), "provider.env")
	key := "test-provider-credential-123456"
	if os.WriteFile(file, []byte("UNRELATED=ignored\nexport ALIYUN_MODEL_STUDIO_AK='"+key+"'\n"), 0600) != nil {
		t.Fatal("write")
	}
	if got, err := loadVoiceKey(file); err != nil || got != key {
		t.Fatal("key not loaded")
	}
	if os.Chmod(file, 0644) != nil {
		t.Fatal("chmod")
	}
	if _, err := loadVoiceKey(file); err == nil {
		t.Fatal("public credential file accepted")
	}
	link := file + "-link"
	if os.Symlink(file, link) != nil {
		t.Fatal("symlink")
	}
	if _, err := loadVoiceKey(link); err == nil {
		t.Fatal("credential symlink accepted")
	}
}
func TestVoiceInputMediaBoundary(t *testing.T) {
	now := time.Now()
	audio := base64.StdEncoding.EncodeToString(make([]byte, 640))
	image := voiceJPEG(t)
	input := newVoiceInput()
	if input.validate(voiceEvent("input_image_buffer.append", map[string]any{"image": image}), now) == nil {
		t.Fatal("image before audio accepted")
	}
	if input.validate(voiceEvent("input_audio_buffer.append", map[string]any{"audio": audio}), now) != nil {
		t.Fatal("valid pcm rejected")
	}
	if input.validate(voiceEvent("input_image_buffer.append", map[string]any{"image": image}), now) != nil {
		t.Fatal("valid image rejected")
	}
	for _, event := range [][]byte{voiceEvent("session.update", map[string]any{"session": map[string]any{"tools": []any{}}}), voiceEvent("input_audio_buffer.append", map[string]any{"audio": "not base64"}), voiceEvent("input_image_buffer.append", map[string]any{"image": audio}), voiceEvent("input_audio_buffer.append", map[string]any{"audio": audio, "authorization": "secret"})} {
		if input.validate(event, now) == nil {
			t.Fatal("invalid media/control accepted")
		}
	}
	burst := newVoiceInput()
	burst.audio = true
	if burst.validate(voiceEvent("input_image_buffer.append", map[string]any{"image": image}), now) != nil || burst.validate(voiceEvent("input_image_buffer.append", map[string]any{"image": image}), now) != nil {
		t.Fatal("image burst allowance")
	}
	if burst.validate(voiceEvent("input_image_buffer.append", map[string]any{"image": image}), now) == nil {
		t.Fatal("image flood accepted")
	}
	if burst.validate(voiceEvent("input_image_buffer.append", map[string]any{"image": image}), now.Add(time.Second)) != nil {
		t.Fatal("image rate did not recover")
	}
}
func TestVoiceControlRateAndCapacity(t *testing.T) {
	now := time.Now()
	input := newVoiceInput()
	input.at = now
	for i := 0; i < 200; i++ {
		if input.validate(voiceEvent("response.cancel", nil), now) != nil {
			t.Fatal("valid control burst rejected")
		}
	}
	if input.validate(voiceEvent("response.cancel", nil), now) == nil {
		t.Fatal("unbounded control events")
	}
	if input.validate(voiceEvent("response.cancel", nil), now.Add(time.Second)) != nil {
		t.Fatal("event rate did not recover")
	}
	p := testVoiceProxy("ws://example.invalid", "test-key")
	defer p.close()
	for _, account := range []string{"a", "b", "c", "d"} {
		if !p.reserve(account, func() {}) {
			t.Fatal("slot unavailable")
		}
	}
	if p.reserve("e", func() {}) {
		t.Fatal("global capacity ignored")
	}
	p.release("a")
	if !p.reserve("e", func() {}) {
		t.Fatal("released slot unavailable")
	}
	p.close()
	if p.reserve("f", func() {}) {
		t.Fatal("closed proxy accepted a call")
	}
}

func testVoiceProxy(endpoint, key string) *voiceProxy {
	return &voiceProxy{cfg: VoiceConfig{Endpoint: endpoint, Model: defaultVoiceModel, Voice: "Ethan"}, key: key, dialer: &websocket.Dialer{HandshakeTimeout: time.Second}, active: map[string]context.CancelFunc{}, maxSession: 5 * time.Second, authInterval: 20 * time.Millisecond}
}
func waitVoice(t *testing.T, condition func() bool) {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if condition() {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("voice cleanup did not finish")
}
func voiceRead(t *testing.T, c *websocket.Conn) map[string]any {
	t.Helper()
	c.SetReadDeadline(time.Now().Add(2 * time.Second))
	var v map[string]any
	if c.ReadJSON(&v) != nil {
		t.Fatal("voice event missing")
	}
	return v
}
func TestVoiceProxyStreamsWithoutLeakingCredentials(t *testing.T) {
	key := "test-provider-credential-123456"
	received := make(chan string, 8)
	var connections atomic.Int32
	provider := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		connections.Add(1)
		if r.Header.Get("Authorization") != "Bearer "+key || r.Header.Get("Cookie") != "" || r.URL.Query().Get("model") != defaultVoiceModel {
			t.Error("provider boundary mismatch")
		}
		c, err := (&websocket.Upgrader{}).Upgrade(w, r, nil)
		if err != nil {
			return
		}
		defer c.Close()
		c.WriteJSON(map[string]any{"type": "session.created"})
		var update map[string]any
		if c.ReadJSON(&update) != nil {
			return
		}
		session, _ := update["session"].(map[string]any)
		if session["input_audio_format"] != "pcm" || session["voice"] != "Ethan" {
			t.Error("provider configuration mismatch")
		}
		c.WriteJSON(map[string]any{"type": "session.updated"})
		for {
			var ev map[string]any
			if c.ReadJSON(&ev) != nil {
				return
			}
			kind, _ := ev["type"].(string)
			received <- kind
			if kind == "input_image_buffer.append" {
				c.WriteJSON(map[string]any{"type": "response.audio.delta", "delta": base64.StdEncoding.EncodeToString(make([]byte, 640))})
				c.WriteJSON(map[string]any{"type": "response.audio_transcript.done", "transcript": "Image received", "private_echo": key})
			}
		}
	}))
	defer provider.Close()
	s, _, _ := fixture(t)
	p := testVoiceProxy("ws"+strings.TrimPrefix(provider.URL, "http"), key)
	s.voice = p
	defer p.close()
	gateway := httptest.NewServer(s)
	defer gateway.Close()
	endpoint := "ws" + strings.TrimPrefix(gateway.URL, "http") + "/voice/v1/realtime"
	if c, _, err := websocket.DefaultDialer.Dial(endpoint, nil); err == nil {
		c.Close()
		t.Fatal("anonymous voice allowed")
	}
	headers := http.Header{"Authorization": []string{"Bearer " + s.cfg.OperatorToken}, "Origin": []string{"https://foreign.example"}}
	if c, _, err := websocket.DefaultDialer.Dial(endpoint, headers); err == nil {
		c.Close()
		t.Fatal("foreign origin allowed")
	}
	if connections.Load() != 0 {
		t.Fatal("unauthorized voice contacted provider")
	}
	headers.Set("Origin", s.cfg.PublicURL)
	client, _, err := websocket.DefaultDialer.Dial(endpoint, headers)
	if err != nil {
		t.Fatal("authorized voice rejected")
	}
	if voiceRead(t, client)["type"] != "session.created" || voiceRead(t, client)["type"] != "session.updated" {
		t.Fatal("voice setup missing")
	}
	if duplicate, _, err := websocket.DefaultDialer.Dial(endpoint, headers); err == nil {
		duplicate.Close()
		t.Fatal("duplicate account voice allowed")
	}
	client.WriteMessage(websocket.TextMessage, voiceEvent("input_audio_buffer.append", map[string]any{"audio": base64.StdEncoding.EncodeToString(make([]byte, 640))}))
	client.WriteMessage(websocket.TextMessage, voiceEvent("input_image_buffer.append", map[string]any{"image": voiceJPEG(t)}))
	if voiceRead(t, client)["type"] != "response.audio.delta" {
		t.Fatal("audio not forwarded")
	}
	final := voiceRead(t, client)
	if final["transcript"] != "Image received" || final["private_echo"] != "[redacted]" {
		t.Fatal("output/credential boundary mismatch")
	}
	if <-received != "input_audio_buffer.append" || <-received != "input_image_buffer.append" {
		t.Fatal("media order mismatch")
	}
	client.Close()
	waitVoice(t, func() bool { p.mu.Lock(); defer p.mu.Unlock(); return len(p.active) == 0 })
}
func TestVoiceRevocationAndShutdownCloseStreams(t *testing.T) {
	provider := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		c, e := (&websocket.Upgrader{}).Upgrade(w, r, nil)
		if e != nil {
			return
		}
		defer c.Close()
		c.WriteJSON(map[string]any{"type": "session.created"})
		var v any
		if c.ReadJSON(&v) != nil {
			return
		}
		c.WriteJSON(map[string]any{"type": "session.updated"})
		for {
			if _, _, e = c.ReadMessage(); e != nil {
				return
			}
		}
	}))
	defer provider.Close()
	p := testVoiceProxy("ws"+strings.TrimPrefix(provider.URL, "http"), "test-provider-credential")
	defer p.close()
	var allowed atomic.Bool
	allowed.Store(true)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		p.serve(w, r, "owner", func(context.Context) bool { return allowed.Load() })
	}))
	defer server.Close()
	connect := func() *websocket.Conn {
		c, _, e := websocket.DefaultDialer.Dial("ws"+strings.TrimPrefix(server.URL, "http"), nil)
		if e != nil {
			t.Fatal("connect")
		}
		voiceRead(t, c)
		voiceRead(t, c)
		return c
	}
	c := connect()
	allowed.Store(false)
	c.SetReadDeadline(time.Now().Add(time.Second))
	if _, _, e := c.ReadMessage(); e == nil {
		t.Fatal("revoked voice remained open")
	}
	c.Close()
	waitVoice(t, func() bool { p.mu.Lock(); defer p.mu.Unlock(); return len(p.active) == 0 })
	allowed.Store(true)
	p.maxSession = 30 * time.Millisecond
	c = connect()
	c.SetReadDeadline(time.Now().Add(time.Second))
	if _, _, e := c.ReadMessage(); e == nil {
		t.Fatal("session deadline left voice open")
	}
	c.Close()
	waitVoice(t, func() bool { p.mu.Lock(); defer p.mu.Unlock(); return len(p.active) == 0 })
	p.maxSession = 5 * time.Second
	c = connect()
	p.close()
	c.SetReadDeadline(time.Now().Add(time.Second))
	if _, _, e := c.ReadMessage(); e == nil {
		t.Fatal("shutdown left voice open")
	}
	c.Close()
	waitVoice(t, func() bool { p.mu.Lock(); defer p.mu.Unlock(); return len(p.active) == 0 })
}
