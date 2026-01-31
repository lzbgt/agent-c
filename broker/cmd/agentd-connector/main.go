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
	"strings"
	"sync"
	"time"

	"agentd-broker/internal/auth"
	"agentd-broker/internal/proto"

	"github.com/gorilla/websocket"
)

func main() {
	var brokerURL = flag.String("broker", "", "broker websocket URL (wss://host:port/v1/agent/connect)")
	var localBase = flag.String("local-agentd", "http://127.0.0.1:8123", "local agentd base url (loopback recommended)")
	var tlsCA = flag.String("tls-ca", "", "CA PEM to verify broker (required for wss)")
	var tlsCert = flag.String("tls-cert", "", "agent client cert PEM (mTLS)")
	var tlsKey = flag.String("tls-key", "", "agent client key PEM (mTLS)")
	var agentCNPfx = flag.String("agent-cn-prefix", "agentd-", "CN prefix used to extract agent id from cert CN")
	var agentID = flag.String("agent-id", "", "agent id (optional; defaults from client cert CN)")
	flag.Parse()

	if strings.TrimSpace(*brokerURL) == "" {
		log.Fatalf("--broker is required")
	}

	u, err := url.Parse(strings.TrimSpace(*brokerURL))
	if err != nil {
		log.Fatalf("invalid broker url: %v", err)
	}

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
	conn, resp, err := dialer.Dial(u.String(), nil)
	if err != nil {
		if resp != nil {
			log.Fatalf("dial failed: %v (http=%d)", err, resp.StatusCode)
		}
		log.Fatalf("dial failed: %v", err)
	}
	defer conn.Close()
	conn.SetReadLimit(64 * 1024 * 1024)

	hello := proto.Hello{
		Type:    proto.TypeHello,
		AgentID: strings.TrimSpace(*agentID),
		Meta: map[string]any{
			"local_base": *localBase,
		},
	}
	if err := conn.WriteJSON(hello); err != nil {
		log.Fatalf("send hello: %v", err)
	}
	var ack proto.HelloAck
	if err := conn.ReadJSON(&ack); err != nil {
		log.Fatalf("read hello_ack: %v", err)
	}
	if !ack.OK {
		log.Fatalf("broker rejected hello: %s", ack.Error)
	}
	if ack.AgentID != "" {
		log.Printf("connected as agent_id=%s", ack.AgentID)
	}

	httpClient := &http.Client{Timeout: 120 * time.Second}
	var streamsMu sync.Mutex
	streamCancels := map[string]context.CancelFunc{}
	for {
		_, raw, err := conn.ReadMessage()
		if err != nil {
			log.Fatalf("read: %v", err)
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
			respMsg := handleLocalAgentd(httpClient, *localBase, rr)
			if err := conn.WriteJSON(respMsg); err != nil {
				log.Fatalf("write response: %v", err)
			}
		case proto.TypeHTTPStreamRequest:
			var sr proto.StreamRequest
			if err := json.Unmarshal(raw, &sr); err != nil {
				continue
			}
			go func(req proto.StreamRequest) {
				ctx, cancel := context.WithCancel(context.Background())
				streamsMu.Lock()
				streamCancels[req.ID] = cancel
				streamsMu.Unlock()
				defer func() {
					streamsMu.Lock()
					delete(streamCancels, req.ID)
					streamsMu.Unlock()
				}()
				handleLocalAgentdStream(ctx, conn, httpClient, *localBase, req)
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

func handleLocalAgentdStream(ctx context.Context, conn *websocket.Conn, httpClient *http.Client, base string, msg proto.StreamRequest) {
	writeJSON := func(v any) {
		_ = conn.WriteJSON(v)
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
