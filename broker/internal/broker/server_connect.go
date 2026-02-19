package broker

import (
	"context"
	"encoding/json"
	"log"
	"net/http"
	"strings"
	"time"

	"agentd-broker/internal/auth"
	"agentd-broker/internal/events"
	"agentd-broker/internal/proto"
	"agentd-broker/internal/registry"
	"agentd-broker/internal/transport"
)

func (s *Server) handleAgentConnect(w http.ResponseWriter, r *http.Request) {
	if r.Method != "GET" {
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	cert, certErr := auth.VerifiedClientLeaf(r)
	if certErr != nil {
		if s.cfg.RequireAgentMTLS {
			writeErrorJSON(w, "agent mTLS required", http.StatusUnauthorized)
			return
		}
	}
	certAgentID := ""
	if cert != nil {
		id, err := auth.AgentIDFromCertCN(cert, s.cfg.AgentCNPfx)
		if err != nil {
			writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
			return
		}
		certAgentID = id
	}

	conn, err := s.upg.Upgrade(w, r, nil)
	if err != nil {
		return
	}
	wsConn := transport.NewWebSocket(conn)
	wsConn.SetReadLimit(64 * 1024 * 1024)
	// Detect dead peers. The connector should answer pings; the read deadline is extended on pong.
	_ = wsConn.SetReadDeadline(time.Now().Add(120 * time.Second))
	wsConn.SetPongHandler(func(string) error {
		_ = wsConn.SetReadDeadline(time.Now().Add(120 * time.Second))
		return nil
	})

	// Handshake: require hello message.
	var hello proto.Hello
	if err := wsConn.ReadJSON(&hello); err != nil {
		_ = wsConn.Close()
		return
	}
	if hello.Type != proto.TypeHello {
		_ = wsConn.Close()
		return
	}
	agentID := strings.TrimSpace(hello.AgentID)
	if agentID == "" {
		agentID = certAgentID
	}
	if agentID == "" {
		_ = wsConn.Close()
		return
	}
	if certAgentID != "" && agentID != certAgentID {
		_ = wsConn.WriteJSON(proto.HelloAck{Type: "hello_ack", OK: false, Error: "agent_id mismatch vs client cert"})
		_ = wsConn.Close()
		return
	}

	enabled, err := s.cfg.DB.AgentEnabled(r.Context(), agentID)
	if err != nil {
		_ = wsConn.WriteJSON(proto.HelloAck{Type: proto.TypeHelloAck, OK: false, Error: "agent not registered"})
		_ = wsConn.Close()
		return
	}
	if !enabled {
		_ = wsConn.WriteJSON(proto.HelloAck{Type: proto.TypeHelloAck, OK: false, Error: "agent disabled"})
		_ = wsConn.Close()
		return
	}

	deploymentID := ""
	if hello.Meta != nil {
		if v, ok := hello.Meta["deployment_id"]; ok {
			if s, ok := v.(string); ok {
				deploymentID = s
			}
		}
		if deploymentID == "" {
			if v, ok := hello.Meta["deployment"]; ok {
				if s, ok := v.(string); ok {
					deploymentID = s
				}
			}
		}
	}
	deploymentID = strings.TrimSpace(deploymentID)
	if deploymentID != "" && !deploymentIDRe.MatchString(deploymentID) {
		log.Printf("invalid deployment_id for agent_id=%s: %q (using default)", agentID, deploymentID)
		deploymentID = ""
	}
	if deploymentID == "" {
		deploymentID = "default"
	}
	if hello.Meta == nil {
		hello.Meta = map[string]any{}
	}
	hello.Meta["deployment_id"] = deploymentID

	connID, err := s.cfg.DB.InsertConnection(r.Context(), agentID, r.RemoteAddr, hello.Meta)
	if err != nil {
		_ = wsConn.WriteJSON(proto.HelloAck{Type: proto.TypeHelloAck, OK: false, Error: "db connection record failed"})
		_ = wsConn.Close()
		return
	}

	ac := &registry.AgentConn{
		AgentID:      agentID,
		DeploymentID: deploymentID,
		Conn:         wsConn,
		Connected:    time.Now(),
		LastSeen:     time.Now(),
		RemoteAddr:   r.RemoteAddr,
		Meta:         hello.Meta,
		DBConnID:     connID,
		PendingLimit: func() int {
			if s.cfg.MaxPendingPerAgent < 0 {
				return 0
			}
			return s.cfg.MaxPendingPerAgent
		}(),
		StreamLimit: func() int {
			if s.cfg.MaxStreamsPerAgent < 0 {
				return 0
			}
			return s.cfg.MaxStreamsPerAgent
		}(),
	}
	ac.InitSession()
	if prev := s.cfg.Registry.Upsert(ac); prev != nil && prev != ac {
		prev.Close()
	}

	_ = wsConn.WriteJSON(proto.HelloAck{Type: proto.TypeHelloAck, OK: true, AgentID: agentID})

	if subs, err := s.cfg.DB.ListAgentMemberSubs(r.Context(), agentID); err == nil {
		s.cfg.Events.PublishTo(subs, events.Event{
			Type:    "agent_connected",
			AgentID: agentID,
			Payload: map[string]any{
				"remote_addr":   r.RemoteAddr,
				"deployment_id": deploymentID,
			},
		})
	}

	// Keep agent websocket alive and surface dead connections promptly.
	go func() {
		t := time.NewTicker(30 * time.Second)
		defer t.Stop()
		for {
			select {
			case <-ac.Done():
				return
			case <-t.C:
				if err := ac.Ping(); err != nil {
					ac.Close()
					return
				}
			}
		}
	}()

	go s.agentReadLoop(ac)
}

func (s *Server) agentReadLoop(a *registry.AgentConn) {
	defer func() {
		if a != nil {
			_ = s.cfg.DB.MarkConnectionDisconnected(context.Background(), a.DBConnID)
			a.Close()
			s.cfg.Registry.Delete(a.AgentID, a.DeploymentID)
			if subs, err := s.cfg.DB.ListAgentMemberSubs(context.Background(), a.AgentID); err == nil {
				s.cfg.Events.PublishTo(subs, events.Event{
					Type:    "agent_disconnected",
					AgentID: a.AgentID,
					Payload: map[string]any{
						"deployment_id": a.DeploymentID,
					},
				})
			}
		}
	}()
	for {
		raw, err := a.Conn.ReadMessage()
		if err != nil {
			return
		}
		a.LastSeen = time.Now()

		var env struct {
			Type string `json:"type"`
		}
		if err := json.Unmarshal(raw, &env); err != nil {
			continue
		}
		switch env.Type {
		case proto.TypeHTTPResp:
			var rr proto.RelayResponse
			if err := json.Unmarshal(raw, &rr); err != nil {
				continue
			}
			a.Deliver(rr)
		case proto.TypeHTTPStreamStart:
			var st proto.StreamStart
			if err := json.Unmarshal(raw, &st); err != nil {
				continue
			}
			if ok := a.DeliverStream(st.ID, st); !ok {
				_ = a.SendAny(proto.StreamCancel{Type: proto.TypeHTTPStreamCancel, ID: st.ID})
			}
		case proto.TypeHTTPStreamChunk:
			var ch proto.StreamChunk
			if err := json.Unmarshal(raw, &ch); err != nil {
				continue
			}
			if ok := a.DeliverStream(ch.ID, ch); !ok {
				_ = a.SendAny(proto.StreamCancel{Type: proto.TypeHTTPStreamCancel, ID: ch.ID})
			}
		case proto.TypeHTTPStreamEnd:
			var en proto.StreamEnd
			if err := json.Unmarshal(raw, &en); err != nil {
				continue
			}
			_ = a.DeliverStream(en.ID, en)
			a.CloseStream(en.ID)
		default:
			// ignore
		}
	}
}
