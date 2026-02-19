package broker

import (
	"context"
	"encoding/base64"
	"testing"
	"time"

	"agentd-broker/internal/events"
	"agentd-broker/internal/proto"
	"agentd-broker/internal/registry"
	"agentd-broker/internal/transport"
)

type fakeRelayConn struct {
	agent  *registry.AgentConn
	status int
	body   []byte
}

func (f *fakeRelayConn) ReadJSON(_ any) error                { return nil }
func (f *fakeRelayConn) ReadMessage() ([]byte, error)        { return nil, nil }
func (f *fakeRelayConn) Close() error                        { return nil }
func (f *fakeRelayConn) SetReadLimit(_ int64)                {}
func (f *fakeRelayConn) SetReadDeadline(_ time.Time) error   { return nil }
func (f *fakeRelayConn) SetPongHandler(_ func(string) error) {}
func (f *fakeRelayConn) Ping() error                         { return nil }
func (f *fakeRelayConn) WriteJSON(v any) error {
	req, ok := v.(proto.RelayRequest)
	if !ok || f.agent == nil {
		return nil
	}
	resp := proto.RelayResponse{
		Type: proto.TypeHTTPResp,
		ID:   req.ID,
		Resp: proto.HTTPResponse{
			Status:  f.status,
			Headers: map[string]string{"Content-Type": "application/json"},
			BodyB64: base64.StdEncoding.EncodeToString(f.body),
		},
	}
	f.agent.Deliver(resp)
	return nil
}

var _ transport.Conn = (*fakeRelayConn)(nil)

func TestExecuteAgentTasks(t *testing.T) {
	reg := registry.New()
	ac := &registry.AgentConn{
		AgentID:      "agent1",
		DeploymentID: "default",
		Connected:    time.Now(),
	}
	fake := &fakeRelayConn{agent: ac, status: 200, body: []byte(`{"ok":true}`)}
	ac.Conn = fake
	ac.InitSession()
	reg.Upsert(ac)

	s := &Server{
		cfg: Config{
			Registry: reg,
			Events:   events.New(),
		},
	}
	p := &Principal{Sub: "user1"}
	tasks := []agentTaskPrepared{
		{TaskID: "t1", AgentID: "agent1", DeploymentID: "default", Method: "POST", Path: "/api/v1/run"},
		{TaskID: "t2", AgentID: "agent1", DeploymentID: "default", Method: "POST", Path: "/api/v1/run"},
	}
	results := s.executeAgentTasks(context.Background(), p, tasks, 2, 1000, "trace-test")
	if len(results) != 2 {
		t.Fatalf("expected 2 results, got %d", len(results))
	}
	for _, r := range results {
		if !r.OK || r.HTTPStatus != 200 {
			t.Fatalf("unexpected task result: %+v", r)
		}
	}
}
