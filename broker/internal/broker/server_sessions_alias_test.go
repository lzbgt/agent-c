package broker

import (
	"encoding/base64"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"agentd-broker/internal/proto"
	"agentd-broker/internal/registry"
	"agentd-broker/internal/transport"
)

type fakeSessionAliasConn struct {
	agent         *registry.AgentConn
	lastRelayReq  *proto.RelayRequest
	lastStreamReq *proto.StreamRequest
}

func (f *fakeSessionAliasConn) ReadJSON(_ any) error                { return nil }
func (f *fakeSessionAliasConn) ReadMessage() ([]byte, error)        { return nil, nil }
func (f *fakeSessionAliasConn) Close() error                        { return nil }
func (f *fakeSessionAliasConn) SetReadLimit(_ int64)                {}
func (f *fakeSessionAliasConn) SetReadDeadline(_ time.Time) error   { return nil }
func (f *fakeSessionAliasConn) SetPongHandler(_ func(string) error) {}
func (f *fakeSessionAliasConn) Ping() error                         { return nil }
func (f *fakeSessionAliasConn) WriteJSON(v any) error {
	switch req := v.(type) {
	case proto.RelayRequest:
		f.lastRelayReq = &req
		body := []byte(`{"ok":true}`)
		if req.Req.Path == "/api/v1/session/attach" {
			body = []byte(`{"ok":true,"session":{"session_id":"sess-123"}}`)
		}
		if req.Req.Path == "/api/v1/session/sess-123/transcript" {
			body = []byte(`{"ok":true,"session_id":"sess-123","entries":[]}`)
		}
		if req.Req.Path == "/api/v1/session/sess-123/orchestration/status" {
			body = []byte(`{"ok":true,"counts":{"workers":2,"services":1}}`)
		}
		if req.Req.Path == "/api/v1/session/sess-123/shells" {
			body = []byte(`{"ok":true,"shells":[{"job_id":"bg-1","label":"build","status":"running"}]}`)
		}
		if req.Req.Path == "/api/v1/session/sess-123/shells/start" {
			body = []byte(`{"ok":true,"job":{"job_id":"bg-2","label":"probe","status":"running"}}`)
		}
		if req.Req.Path == "/api/v1/session/sess-123/shells/@api.http/poll" {
			body = []byte(`{"ok":true,"job":{"job_id":"bg-1","status":"running","stdout":"ready"}}`)
		}
		if req.Req.Path == "/api/v1/session/sess-123/services" {
			body = []byte(`{"ok":true,"services":[{"job_id":"bg-7","label":"api","status":"ready"}]}`)
		}
		if req.Req.Path == "/api/v1/session/sess-123/services/@api.http/run" {
			body = []byte(`{"ok":true,"result":{"status":"ok"}}`)
		}
		if req.Req.Path == "/api/v1/session/sess-123/capabilities/@api.http" {
			body = []byte(`{"ok":true,"capability":{"name":"@api.http","providers":["bg-7"]}}`)
		}
		f.agent.Deliver(proto.RelayResponse{
			Type: proto.TypeHTTPResp,
			ID:   req.ID,
			Resp: proto.HTTPResponse{
				Status:  http.StatusOK,
				Headers: map[string]string{"Content-Type": "application/json"},
				BodyB64: base64.StdEncoding.EncodeToString(body),
			},
		})
	case proto.StreamRequest:
		f.lastStreamReq = &req
		f.agent.DeliverStream(req.ID, proto.StreamStart{
			Type: proto.TypeHTTPStreamStart,
			ID:   req.ID,
			Resp: proto.HTTPResponse{
				Status:  http.StatusOK,
				Headers: map[string]string{"Content-Type": "text/event-stream"},
			},
		})
		f.agent.DeliverStream(req.ID, proto.StreamChunk{
			Type: proto.TypeHTTPStreamChunk,
			ID:   req.ID,
			Data: base64.StdEncoding.EncodeToString([]byte("id: 41\nevent: agent_event\ndata: {\"type\":\"message\",\"text\":\"hello\"}\n\n")),
		})
		f.agent.DeliverStream(req.ID, proto.StreamEnd{
			Type: proto.TypeHTTPStreamEnd,
			ID:   req.ID,
		})
	}
	return nil
}

var _ transport.Conn = (*fakeSessionAliasConn)(nil)

func newSessionAliasTestServer() (*Server, *fakeSessionAliasConn) {
	reg := registry.New()
	ac := &registry.AgentConn{
		AgentID:      "agent1",
		DeploymentID: "default",
		Connected:    time.Now(),
		LastSeen:     time.Now(),
	}
	fake := &fakeSessionAliasConn{agent: ac}
	ac.Conn = fake
	ac.InitSession()
	reg.Upsert(ac)

	s := newTestAuthServer("test-token")
	s.cfg.Registry = reg
	s.cfg.MaxRequestBodySize = 1024 * 1024
	return s, fake
}

func TestHandleAgentSessionAttachAliasInjectsSessionID(t *testing.T) {
	s, fake := newSessionAliasTestServer()
	req := httptest.NewRequest("POST", "http://broker/v1/agents/agent1/sessions/sess-123/attach", strings.NewReader(`{"thread_id":"thread-1"}`))
	req.Header.Set("Authorization", "Bearer test-token")
	req.Header.Set("Content-Type", "application/json")
	w := httptest.NewRecorder()

	s.handleAgentsSubroutes(w, req)

	res := w.Result()
	defer res.Body.Close()
	if res.StatusCode != http.StatusOK {
		t.Fatalf("expected 200, got %d", res.StatusCode)
	}
	if fake.lastRelayReq == nil {
		t.Fatalf("expected relay request")
	}
	if fake.lastRelayReq.Req.Path != "/api/v1/session/attach" {
		t.Fatalf("unexpected path %q", fake.lastRelayReq.Req.Path)
	}
	rawBody, err := base64.StdEncoding.DecodeString(fake.lastRelayReq.Req.BodyB64)
	if err != nil {
		t.Fatalf("decode proxied body: %v", err)
	}
	var payload map[string]any
	if err := json.Unmarshal(rawBody, &payload); err != nil {
		t.Fatalf("unmarshal proxied body: %v", err)
	}
	if got := strings.TrimSpace(anyToString(payload["session_id"])); got != "sess-123" {
		t.Fatalf("expected session_id injection, got %#v", payload["session_id"])
	}
	if got := strings.TrimSpace(anyToString(payload["thread_id"])); got != "thread-1" {
		t.Fatalf("expected thread_id passthrough, got %#v", payload["thread_id"])
	}
}

func TestHandleAgentSessionEventsAliasForwardsLastEventID(t *testing.T) {
	s, fake := newSessionAliasTestServer()
	req := httptest.NewRequest("GET", "http://broker/v1/agents/agent1/sessions/sess-123/events", nil)
	req.Header.Set("Authorization", "Bearer test-token")
	req.Header.Set("Last-Event-ID", "41")
	w := httptest.NewRecorder()

	s.handleAgentsSubroutes(w, req)

	res := w.Result()
	defer res.Body.Close()
	if res.StatusCode != http.StatusOK {
		t.Fatalf("expected 200, got %d", res.StatusCode)
	}
	if fake.lastStreamReq == nil {
		t.Fatalf("expected stream request")
	}
	if fake.lastStreamReq.Req.Path != "/api/v1/session/sess-123/events" {
		t.Fatalf("unexpected stream path %q", fake.lastStreamReq.Req.Path)
	}
	if got := fake.lastStreamReq.Req.Headers["Last-Event-ID"]; got != "41" {
		t.Fatalf("expected Last-Event-ID forwarded, got %q", got)
	}
	if ct := res.Header.Get("Content-Type"); !strings.Contains(ct, "text/event-stream") {
		t.Fatalf("expected text/event-stream content type, got %q", ct)
	}
	if body := w.Body.String(); !strings.Contains(body, "id: 41") || !strings.Contains(body, "\"hello\"") {
		t.Fatalf("unexpected SSE body %q", body)
	}
}

func TestHandleAgentSessionTranscriptAliasRelaysToTranscriptPath(t *testing.T) {
	s, fake := newSessionAliasTestServer()
	req := httptest.NewRequest("GET", "http://broker/v1/agents/agent1/sessions/sess-123/transcript", nil)
	req.Header.Set("Authorization", "Bearer test-token")
	w := httptest.NewRecorder()

	s.handleAgentsSubroutes(w, req)

	res := w.Result()
	defer res.Body.Close()
	if res.StatusCode != http.StatusOK {
		t.Fatalf("expected 200, got %d", res.StatusCode)
	}
	if fake.lastRelayReq == nil {
		t.Fatalf("expected relay request")
	}
	if fake.lastRelayReq.Req.Path != "/api/v1/session/sess-123/transcript" {
		t.Fatalf("unexpected transcript path %q", fake.lastRelayReq.Req.Path)
	}
}

func TestHandleAgentSessionOrchestrationAliasRelaysToStatusPath(t *testing.T) {
	s, fake := newSessionAliasTestServer()
	req := httptest.NewRequest("GET", "http://broker/v1/agents/agent1/sessions/sess-123/orchestration/status", nil)
	req.Header.Set("Authorization", "Bearer test-token")
	w := httptest.NewRecorder()

	s.handleAgentsSubroutes(w, req)

	res := w.Result()
	defer res.Body.Close()
	if res.StatusCode != http.StatusOK {
		t.Fatalf("expected 200, got %d", res.StatusCode)
	}
	if fake.lastRelayReq == nil {
		t.Fatalf("expected relay request")
	}
	if fake.lastRelayReq.Req.Path != "/api/v1/session/sess-123/orchestration/status" {
		t.Fatalf("unexpected orchestration path %q", fake.lastRelayReq.Req.Path)
	}
}

func TestHandleAgentSessionShellStartAliasMapsToStartPath(t *testing.T) {
	s, fake := newSessionAliasTestServer()
	req := httptest.NewRequest("POST", "http://broker/v1/agents/agent1/sessions/sess-123/shells", strings.NewReader(`{"command":"pwd","label":"probe"}`))
	req.Header.Set("Authorization", "Bearer test-token")
	req.Header.Set("Content-Type", "application/json")
	w := httptest.NewRecorder()

	s.handleAgentsSubroutes(w, req)

	res := w.Result()
	defer res.Body.Close()
	if res.StatusCode != http.StatusOK {
		t.Fatalf("expected 200, got %d", res.StatusCode)
	}
	if fake.lastRelayReq == nil {
		t.Fatalf("expected relay request")
	}
	if fake.lastRelayReq.Req.Path != "/api/v1/session/sess-123/shells/start" {
		t.Fatalf("unexpected shell start path %q", fake.lastRelayReq.Req.Path)
	}
	rawBody, err := base64.StdEncoding.DecodeString(fake.lastRelayReq.Req.BodyB64)
	if err != nil {
		t.Fatalf("decode proxied body: %v", err)
	}
	if got := string(rawBody); !strings.Contains(got, `"command":"pwd"`) || !strings.Contains(got, `"label":"probe"`) {
		t.Fatalf("unexpected shell start body %q", got)
	}
}

func TestHandleAgentSessionShellActionAliasDecodesJobRef(t *testing.T) {
	s, fake := newSessionAliasTestServer()
	req := httptest.NewRequest("POST", "http://broker/v1/agents/agent1/sessions/sess-123/shells/%40api.http/poll", strings.NewReader(`{}`))
	req.Header.Set("Authorization", "Bearer test-token")
	req.Header.Set("Content-Type", "application/json")
	w := httptest.NewRecorder()

	s.handleAgentsSubroutes(w, req)

	res := w.Result()
	defer res.Body.Close()
	if res.StatusCode != http.StatusOK {
		t.Fatalf("expected 200, got %d", res.StatusCode)
	}
	if fake.lastRelayReq == nil {
		t.Fatalf("expected relay request")
	}
	if fake.lastRelayReq.Req.Path != "/api/v1/session/sess-123/shells/@api.http/poll" {
		t.Fatalf("unexpected shell action path %q", fake.lastRelayReq.Req.Path)
	}
}

func TestHandleAgentSessionServiceRunAliasDecodesJobRef(t *testing.T) {
	s, fake := newSessionAliasTestServer()
	req := httptest.NewRequest("POST", "http://broker/v1/agents/agent1/sessions/sess-123/services/%40api.http/run", strings.NewReader(`{"recipe":"health"}`))
	req.Header.Set("Authorization", "Bearer test-token")
	req.Header.Set("Content-Type", "application/json")
	w := httptest.NewRecorder()

	s.handleAgentsSubroutes(w, req)

	res := w.Result()
	defer res.Body.Close()
	if res.StatusCode != http.StatusOK {
		t.Fatalf("expected 200, got %d", res.StatusCode)
	}
	if fake.lastRelayReq == nil {
		t.Fatalf("expected relay request")
	}
	if fake.lastRelayReq.Req.Path != "/api/v1/session/sess-123/services/@api.http/run" {
		t.Fatalf("unexpected service run path %q", fake.lastRelayReq.Req.Path)
	}
}

func TestHandleAgentSessionCapabilityAliasDecodesCapabilityRef(t *testing.T) {
	s, fake := newSessionAliasTestServer()
	req := httptest.NewRequest("GET", "http://broker/v1/agents/agent1/sessions/sess-123/capabilities/%40api.http", nil)
	req.Header.Set("Authorization", "Bearer test-token")
	w := httptest.NewRecorder()

	s.handleAgentsSubroutes(w, req)

	res := w.Result()
	defer res.Body.Close()
	if res.StatusCode != http.StatusOK {
		t.Fatalf("expected 200, got %d", res.StatusCode)
	}
	if fake.lastRelayReq == nil {
		t.Fatalf("expected relay request")
	}
	if fake.lastRelayReq.Req.Path != "/api/v1/session/sess-123/capabilities/@api.http" {
		t.Fatalf("unexpected capability path %q", fake.lastRelayReq.Req.Path)
	}
}

func anyToString(v any) string {
	switch value := v.(type) {
	case string:
		return value
	default:
		return ""
	}
}
