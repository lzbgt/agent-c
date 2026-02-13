package main

import (
	"bytes"
	"context"
	"crypto/tls"
	"crypto/x509"
	"encoding/base64"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"os/signal"
	"strings"
	"sync"
	"syscall"
	"time"

	"agentd-broker/internal/auth"
	"agentd-broker/internal/proto"

	"github.com/gorilla/websocket"
)

type safeWS struct {
	conn *websocket.Conn
	mu   sync.Mutex
}

func (s *safeWS) WriteJSON(v any) error {
	if s == nil || s.conn == nil {
		return errors.New("nil websocket")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.conn.WriteJSON(v)
}

func (s *safeWS) WriteControl(messageType int, data []byte, deadline time.Time) error {
	if s == nil || s.conn == nil {
		return errors.New("nil websocket")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.conn.WriteControl(messageType, data, deadline)
}

func (s *safeWS) Close() error {
	if s == nil || s.conn == nil {
		return nil
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.conn.Close()
}

func main() {
	var brokerURL = flag.String("broker", "", "broker websocket URL (wss://host:port/v1/agent/connect)")
	var localBase = flag.String("local-agentd", "http://127.0.0.1:8123", "local agentd base url (loopback recommended)")
	var tlsCA = flag.String("tls-ca", "", "CA PEM to verify broker (required for wss)")
	var tlsCert = flag.String("tls-cert", "", "agent client cert PEM (mTLS)")
	var tlsKey = flag.String("tls-key", "", "agent client key PEM (mTLS)")
	var agentCNPfx = flag.String("agent-cn-prefix", "agentd-", "CN prefix used to extract agent id from cert CN")
	var agentID = flag.String("agent-id", "", "agent id (optional; defaults from client cert CN)")
	var deploymentID = flag.String("deployment-id", "", "deployment id (optional; enables multi-deployment per agent)")
	flag.Parse()

	if strings.TrimSpace(*brokerURL) == "" {
		log.Fatalf("--broker is required")
	}

	u, err := url.Parse(strings.TrimSpace(*brokerURL))
	if err != nil {
		log.Fatalf("invalid broker url: %v", err)
	}

	if strings.TrimSpace(*deploymentID) == "" {
		if v := strings.TrimSpace(os.Getenv("AGENTD_DEPLOYMENT_ID")); v != "" {
			*deploymentID = v
		}
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	tlsConfig := &tls.Config{MinVersion: tls.VersionTLS12}
	if u.Scheme == "wss" {
		if strings.TrimSpace(*tlsCA) == "" {
			log.Fatalf("--tls-ca required for wss")
		}
		cab, err := os.ReadFile(strings.TrimSpace(*tlsCA))
		if err != nil {
			log.Fatalf("read tls ca: %v", err)
		}
		pool := x509.NewCertPool()
		if !pool.AppendCertsFromPEM(cab) {
			log.Fatalf("bad tls ca pem: %s", *tlsCA)
		}
		tlsConfig.RootCAs = pool

		if strings.TrimSpace(*tlsCert) == "" || strings.TrimSpace(*tlsKey) == "" {
			log.Fatalf("--tls-cert and --tls-key required for mTLS agent connection")
		}
		cert, err := tls.LoadX509KeyPair(strings.TrimSpace(*tlsCert), strings.TrimSpace(*tlsKey))
		if err != nil {
			log.Fatalf("load agent cert/key: %v", err)
		}
		tlsConfig.Certificates = []tls.Certificate{cert}

		// Best-effort infer agent id from cert CN if not provided.
		if strings.TrimSpace(*agentID) == "" && len(cert.Certificate) > 0 {
			x, err := x509.ParseCertificate(cert.Certificate[0])
			if err == nil {
				id, err := auth.AgentIDFromCertCN(x, *agentCNPfx)
				if err == nil {
					*agentID = id
				}
			}
		}
	}

	dialer := websocket.Dialer{TLSClientConfig: tlsConfig}
	httpClient := &http.Client{Timeout: 120 * time.Second}

	backoff := 250 * time.Millisecond
	for {
		select {
		case <-ctx.Done():
			return
		default:
		}

		conn, resp, err := dialer.Dial(u.String(), nil)
		if err != nil {
			if resp != nil {
				log.Printf("dial failed: %v (http=%d)", err, resp.StatusCode)
			} else {
				log.Printf("dial failed: %v", err)
			}
			time.Sleep(backoff)
			if backoff < 10*time.Second {
				backoff *= 2
			}
			continue
		}
		backoff = 250 * time.Millisecond

		conn.SetReadLimit(64 * 1024 * 1024)
		_ = conn.SetReadDeadline(time.Now().Add(120 * time.Second))
		conn.SetPongHandler(func(string) error {
			_ = conn.SetReadDeadline(time.Now().Add(120 * time.Second))
			return nil
		})

		ws := &safeWS{conn: conn}
		err = runSession(ctx, ws, conn, httpClient, strings.TrimSpace(*localBase), strings.TrimSpace(*agentID), strings.TrimSpace(*deploymentID))
		_ = ws.Close()
		if err != nil {
			log.Printf("session ended: %v", err)
		}
		time.Sleep(backoff)
		if backoff < 10*time.Second {
			backoff *= 2
		}
	}
}

func runSession(ctx context.Context, ws *safeWS, conn *websocket.Conn, httpClient *http.Client, localBase, agentID, deploymentID string) error {
	if ws == nil || conn == nil {
		return errors.New("nil websocket connection")
	}
	if httpClient == nil {
		return errors.New("nil http client")
	}

	hello := proto.Hello{
		Type:    proto.TypeHello,
		AgentID: strings.TrimSpace(agentID),
		Meta: map[string]any{
			"local_base": localBase,
		},
	}
	if strings.TrimSpace(deploymentID) != "" {
		hello.Meta["deployment_id"] = strings.TrimSpace(deploymentID)
	}
	if err := ws.WriteJSON(hello); err != nil {
		return fmt.Errorf("send hello: %w", err)
	}
	var ack proto.HelloAck
	if err := conn.ReadJSON(&ack); err != nil {
		return fmt.Errorf("read hello_ack: %w", err)
	}
	if !ack.OK {
		return fmt.Errorf("broker rejected hello: %s", ack.Error)
	}
	if ack.AgentID != "" {
		log.Printf("connected as agent_id=%s", ack.AgentID)
	}

	sessionDone := make(chan struct{})
	defer close(sessionDone)

	// Close the websocket when the process is terminating.
	go func() {
		<-ctx.Done()
		_ = ws.Close()
	}()

	// Keep broker websocket alive.
	ping := time.NewTicker(30 * time.Second)
	defer ping.Stop()
	go func() {
		for {
			select {
			case <-ctx.Done():
				return
			case <-sessionDone:
				return
			case <-ping.C:
				_ = ws.WriteControl(websocket.PingMessage, []byte("ping"), time.Now().Add(5*time.Second))
			}
		}
	}()

	var streamsMu sync.Mutex
	streamCancels := map[string]context.CancelFunc{}
	defer func() {
		streamsMu.Lock()
		for _, cancel := range streamCancels {
			if cancel != nil {
				cancel()
			}
		}
		streamsMu.Unlock()
	}()

	for {
		_, raw, err := conn.ReadMessage()
		if err != nil {
			return fmt.Errorf("read: %w", err)
		}
		var env struct {
			Type string `json:"type"`
		}
		if err := json.Unmarshal(raw, &env); err != nil {
			continue
		}
		switch env.Type {
		case proto.TypeHTTPRequest:
			var rr proto.RelayRequest
			if err := json.Unmarshal(raw, &rr); err != nil {
				continue
			}
			respMsg := handleLocalAgentd(httpClient, localBase, rr)
			if err := ws.WriteJSON(respMsg); err != nil {
				return fmt.Errorf("write response: %w", err)
			}
		case proto.TypeHTTPStreamRequest:
			var sr proto.StreamRequest
			if err := json.Unmarshal(raw, &sr); err != nil {
				continue
			}
			go func(req proto.StreamRequest) {
				sctx, cancel := context.WithCancel(context.Background())
				streamsMu.Lock()
				streamCancels[req.ID] = cancel
				streamsMu.Unlock()
				defer func() {
					streamsMu.Lock()
					delete(streamCancels, req.ID)
					streamsMu.Unlock()
				}()
				handleLocalAgentdStream(sctx, ws, httpClient, localBase, req)
			}(sr)
		case proto.TypeHTTPStreamCancel:
			var cc proto.StreamCancel
			if err := json.Unmarshal(raw, &cc); err != nil {
				continue
			}
			streamsMu.Lock()
			cancel := streamCancels[cc.ID]
			streamsMu.Unlock()
			if cancel != nil {
				cancel()
			}
		default:
			// ignore
		}
	}
}

func handleLocalAgentd(httpClient *http.Client, base string, msg proto.RelayRequest) proto.RelayResponse {
	out := proto.RelayResponse{
		Type: proto.TypeHTTPResp,
		ID:   msg.ID,
	}

	body, err := base64.StdEncoding.DecodeString(msg.Req.BodyB64)
	if err != nil {
		out.Err = "invalid body_b64"
		return out
	}

	u := strings.TrimRight(base, "/") + msg.Req.Path
	if msg.Req.Query != "" {
		u = u + "?" + msg.Req.Query
	}

	req, err := http.NewRequest(msg.Req.Method, u, bytes.NewReader(body))
	if err != nil {
		out.Err = fmt.Sprintf("build request failed: %v", err)
		return out
	}
	for k, v := range msg.Req.Headers {
		if strings.TrimSpace(k) == "" {
			continue
		}
		req.Header.Set(k, v)
	}
	resp, err := httpClient.Do(req)
	if err != nil {
		out.Err = fmt.Sprintf("local agentd request failed: %v", err)
		return out
	}
	defer resp.Body.Close()

	respBody, _ := io.ReadAll(resp.Body)
	hdrs := map[string]string{}
	for k, vv := range resp.Header {
		if len(vv) == 0 {
			continue
		}
		hdrs[k] = vv[0]
	}
	out.Resp = proto.HTTPResponse{
		Status:  resp.StatusCode,
		Headers: hdrs,
		BodyB64: base64.StdEncoding.EncodeToString(respBody),
	}
	return out
}

func handleLocalAgentdStream(ctx context.Context, ws *safeWS, httpClient *http.Client, base string, msg proto.StreamRequest) {
	writeJSON := func(v any) {
		_ = ws.WriteJSON(v)
	}

	body, err := base64.StdEncoding.DecodeString(msg.Req.BodyB64)
	if err != nil {
		writeJSON(proto.StreamEnd{Type: proto.TypeHTTPStreamEnd, ID: msg.ID, Err: "invalid body_b64"})
		return
	}

	u := strings.TrimRight(base, "/") + msg.Req.Path
	if msg.Req.Query != "" {
		u = u + "?" + msg.Req.Query
	}

	req, err := http.NewRequestWithContext(ctx, msg.Req.Method, u, bytes.NewReader(body))
	if err != nil {
		writeJSON(proto.StreamEnd{Type: proto.TypeHTTPStreamEnd, ID: msg.ID, Err: fmt.Sprintf("build request failed: %v", err)})
		return
	}
	for k, v := range msg.Req.Headers {
		if strings.TrimSpace(k) == "" {
			continue
		}
		req.Header.Set(k, v)
	}

	resp, err := httpClient.Do(req)
	if err != nil {
		writeJSON(proto.StreamEnd{Type: proto.TypeHTTPStreamEnd, ID: msg.ID, Err: fmt.Sprintf("local agentd request failed: %v", err)})
		return
	}
	defer resp.Body.Close()

	hdrs := map[string]string{}
	for k, vv := range resp.Header {
		if len(vv) == 0 {
			continue
		}
		hdrs[k] = vv[0]
	}
	writeJSON(proto.StreamStart{
		Type: proto.TypeHTTPStreamStart,
		ID:   msg.ID,
		Resp: proto.HTTPResponse{
			Status:  resp.StatusCode,
			Headers: hdrs,
		},
	})

	buf := make([]byte, 16*1024)
	for {
		n, rerr := resp.Body.Read(buf)
		if n > 0 {
			writeJSON(proto.StreamChunk{
				Type: proto.TypeHTTPStreamChunk,
				ID:   msg.ID,
				Data: base64.StdEncoding.EncodeToString(buf[:n]),
			})
		}
		if rerr == nil {
			continue
		}
		if errors.Is(rerr, io.EOF) {
			writeJSON(proto.StreamEnd{Type: proto.TypeHTTPStreamEnd, ID: msg.ID})
			return
		}
		writeJSON(proto.StreamEnd{Type: proto.TypeHTTPStreamEnd, ID: msg.ID, Err: rerr.Error()})
		return
	}
}
