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

func TestAudioSessionListGetDelete(t *testing.T) {
	token := "test-token"
	s := newTestAudioServer(token)
	ts := httptest.NewServer(s.Handler())
	defer ts.Close()

	createBody := []byte(`{"agent_id":"a-1","deployment_id":"lab","mode":"webrtc"}`)
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
		t.Fatalf("unexpected create status: %d", createResp.StatusCode)
	}
	var created struct {
		OK        bool   `json:"ok"`
		SessionID string `json:"session_id"`
	}
	if err := json.NewDecoder(createResp.Body).Decode(&created); err != nil {
		t.Fatalf("decode create: %v", err)
	}
	if !created.OK || created.SessionID == "" {
		t.Fatalf("invalid create response: %+v", created)
	}

	signalBody := []byte(`{"type":"control","payload":{"state":"ready"}}`)
	signalReq, err := http.NewRequest("POST", ts.URL+"/v1/audio/sessions/"+created.SessionID+"/signal", bytes.NewReader(signalBody))
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

	listReq, err := http.NewRequest("GET", ts.URL+"/v1/audio/sessions?agent_id=a-1&deployment_id=lab", nil)
	if err != nil {
		t.Fatalf("list request: %v", err)
	}
	listReq.Header.Set("Authorization", "Bearer "+token)
	listResp, err := http.DefaultClient.Do(listReq)
	if err != nil {
		t.Fatalf("list request failed: %v", err)
	}
	defer listResp.Body.Close()
	if listResp.StatusCode != http.StatusOK {
		t.Fatalf("unexpected list status: %d", listResp.StatusCode)
	}
	var listPayload struct {
		OK       bool               `json:"ok"`
		Count    int                `json:"count"`
		Sessions []audioSessionInfo `json:"sessions"`
	}
	if err := json.NewDecoder(listResp.Body).Decode(&listPayload); err != nil {
		t.Fatalf("decode list: %v", err)
	}
	if !listPayload.OK || listPayload.Count != 1 || len(listPayload.Sessions) != 1 {
		t.Fatalf("unexpected list payload: %+v", listPayload)
	}
	if listPayload.Sessions[0].SignalCount != 1 || listPayload.Sessions[0].LastSignalType != "control" {
		t.Fatalf("unexpected list session status: %+v", listPayload.Sessions[0])
	}

	getReq, err := http.NewRequest("GET", ts.URL+"/v1/audio/sessions/"+created.SessionID, nil)
	if err != nil {
		t.Fatalf("get request: %v", err)
	}
	getReq.Header.Set("Authorization", "Bearer "+token)
	getResp, err := http.DefaultClient.Do(getReq)
	if err != nil {
		t.Fatalf("get request failed: %v", err)
	}
	defer getResp.Body.Close()
	if getResp.StatusCode != http.StatusOK {
		t.Fatalf("unexpected get status: %d", getResp.StatusCode)
	}
	var getPayload struct {
		OK      bool             `json:"ok"`
		Session audioSessionInfo `json:"session"`
	}
	if err := json.NewDecoder(getResp.Body).Decode(&getPayload); err != nil {
		t.Fatalf("decode get: %v", err)
	}
	if !getPayload.OK || getPayload.Session.SessionID != created.SessionID {
		t.Fatalf("unexpected get payload: %+v", getPayload)
	}
	if getPayload.Session.DeploymentID != "lab" || getPayload.Session.Mode != "webrtc" {
		t.Fatalf("unexpected session detail: %+v", getPayload.Session)
	}

	deleteReq, err := http.NewRequest("DELETE", ts.URL+"/v1/audio/sessions/"+created.SessionID, nil)
	if err != nil {
		t.Fatalf("delete request: %v", err)
	}
	deleteReq.Header.Set("Authorization", "Bearer "+token)
	deleteResp, err := http.DefaultClient.Do(deleteReq)
	if err != nil {
		t.Fatalf("delete request failed: %v", err)
	}
	defer deleteResp.Body.Close()
	if deleteResp.StatusCode != http.StatusOK {
		t.Fatalf("unexpected delete status: %d", deleteResp.StatusCode)
	}
	var deletePayload struct {
		OK        bool   `json:"ok"`
		Deleted   bool   `json:"deleted"`
		SessionID string `json:"session_id"`
	}
	if err := json.NewDecoder(deleteResp.Body).Decode(&deletePayload); err != nil {
		t.Fatalf("decode delete: %v", err)
	}
	if !deletePayload.OK || !deletePayload.Deleted || deletePayload.SessionID != created.SessionID {
		t.Fatalf("unexpected delete payload: %+v", deletePayload)
	}

	getGoneReq, err := http.NewRequest("GET", ts.URL+"/v1/audio/sessions/"+created.SessionID, nil)
	if err != nil {
		t.Fatalf("get gone request: %v", err)
	}
	getGoneReq.Header.Set("Authorization", "Bearer "+token)
	getGoneResp, err := http.DefaultClient.Do(getGoneReq)
	if err != nil {
		t.Fatalf("get gone request failed: %v", err)
	}
	defer getGoneResp.Body.Close()
	if getGoneResp.StatusCode != http.StatusNotFound {
		t.Fatalf("expected 404 after delete, got %d", getGoneResp.StatusCode)
	}
}
