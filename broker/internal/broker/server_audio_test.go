package broker

import (
	"bufio"
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"agentd-broker/internal/auth"
)

func newTestAudioServer(token string) *Server {
	ca := &auth.ClientAuth{ByToken: map[string]*auth.ClientPolicy{}}
	ca.ByToken[token] = &auth.ClientPolicy{
		ClientID: "test-client",
		Token:    token,
		Admin:    true,
	}
	s := &Server{
		cfg: Config{
			ClientAuth:      ca,
			AudioSessionTTL: time.Minute,
		},
		startTime: time.Now(),
	}
	s.clientAuth = ca
	s.audioStore = newAudioSessionStore(time.Minute)
	return s
}

func TestAudioSessionSignalLoopback(t *testing.T) {
	token := "test-token"
	s := newTestAudioServer(token)
	ts := httptest.NewServer(s.Handler())
	defer ts.Close()

	createBody := []byte(`{"agent_id":"a-1","mode":"webrtc"}`)
	createReq, err := http.NewRequest("POST", ts.URL+"/v1/audio/sessions", bytes.NewReader(createBody))
	if err != nil {
		t.Fatalf("create request: %v", err)
	}
	createReq.Header.Set("Authorization", "Bearer "+token)
	createReq.Header.Set("Content-Type", "application/json")
	createResp, err := http.DefaultClient.Do(createReq)
	if err != nil {
		t.Fatalf("create request failed: %v", err)
	}
	defer createResp.Body.Close()
	if createResp.StatusCode != http.StatusOK {
		t.Fatalf("unexpected status: %d", createResp.StatusCode)
	}
	var createPayload struct {
		OK        bool   `json:"ok"`
		SessionID string `json:"session_id"`
	}
	if err := json.NewDecoder(createResp.Body).Decode(&createPayload); err != nil {
		t.Fatalf("decode create: %v", err)
	}
	if !createPayload.OK || createPayload.SessionID == "" {
		t.Fatalf("invalid create response")
	}

	streamReq, err := http.NewRequest("GET", ts.URL+"/v1/audio/sessions/"+createPayload.SessionID+"/signal/stream", nil)
	if err != nil {
		t.Fatalf("stream request: %v", err)
	}
	streamReq.Header.Set("Authorization", "Bearer "+token)
	streamResp, err := http.DefaultClient.Do(streamReq)
	if err != nil {
		t.Fatalf("stream request failed: %v", err)
	}
	defer streamResp.Body.Close()
	if streamResp.StatusCode != http.StatusOK {
		t.Fatalf("unexpected stream status: %d", streamResp.StatusCode)
	}

	signalBody := []byte(`{"type":"offer","payload":{"sdp":"dummy"}}`)
	signalReq, err := http.NewRequest("POST", ts.URL+"/v1/audio/sessions/"+createPayload.SessionID+"/signal", bytes.NewReader(signalBody))
	if err != nil {
		t.Fatalf("signal request: %v", err)
	}
	signalReq.Header.Set("Authorization", "Bearer "+token)
	signalReq.Header.Set("Content-Type", "application/json")
	signalResp, err := http.DefaultClient.Do(signalReq)
	if err != nil {
		t.Fatalf("signal request failed: %v", err)
	}
	signalResp.Body.Close()
	if signalResp.StatusCode != http.StatusOK {
		t.Fatalf("unexpected signal status: %d", signalResp.StatusCode)
	}

	reader := bufio.NewReader(streamResp.Body)
	deadline := time.Now().Add(2 * time.Second)
	var dataLine string
	for time.Now().Before(deadline) {
		line, err := reader.ReadString('\n')
		if err != nil {
			break
		}
		line = strings.TrimSpace(line)
		if strings.HasPrefix(line, "data: ") {
			dataLine = strings.TrimSpace(strings.TrimPrefix(line, "data: "))
			break
		}
	}
	if dataLine == "" {
		t.Fatalf("missing signal data")
	}
	var ev audioSignalEvent
	if err := json.Unmarshal([]byte(dataLine), &ev); err != nil {
		t.Fatalf("decode signal: %v", err)
	}
	if ev.Type != "offer" {
		t.Fatalf("unexpected event type: %s", ev.Type)
	}
	if ev.From != "agentd" {
		t.Fatalf("unexpected event from: %s", ev.From)
	}
}
