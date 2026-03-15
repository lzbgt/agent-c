package broker

import (
	"encoding/json"
	"net/http"
	"strings"
	"time"
)

var audioSessionIDRe = agentIDRe

func (s *Server) handleAudioSessions(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case "POST":
		s.handleAudioSessionCreate(w, r)
	case "GET":
		s.handleAudioSessionList(w, r)
	default:
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
	}
}

func (s *Server) handleAudioSessionsSubroutes(w http.ResponseWriter, r *http.Request) {
	rest := strings.TrimPrefix(r.URL.Path, "/v1/audio/sessions/")
	parts := strings.SplitN(rest, "/", 3)
	sessionID := parts[0]
	if !audioSessionIDRe.MatchString(sessionID) {
		writeErrorJSON(w, "invalid session_id", http.StatusBadRequest)
		return
	}
	if len(parts) == 1 || strings.TrimSpace(parts[1]) == "" {
		switch r.Method {
		case "GET":
			s.handleAudioSessionGet(w, r, sessionID)
		case "DELETE":
			s.handleAudioSessionDelete(w, r, sessionID)
		default:
			writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
		}
		return
	}
	action := parts[1]
	if action == "signal" {
		if len(parts) == 2 {
			s.handleAudioSignalSend(w, r, sessionID)
			return
		}
		if parts[2] == "stream" {
			s.handleAudioSignalStream(w, r, sessionID)
			return
		}
	}
	writeErrorJSON(w, "not found", http.StatusNotFound)
}

func (s *Server) handleAudioSessionList(w http.ResponseWriter, r *http.Request) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	agentID := strings.TrimSpace(r.URL.Query().Get("agent_id"))
	if agentID != "" && !agentIDRe.MatchString(agentID) {
		writeErrorJSON(w, "invalid agent_id", http.StatusBadRequest)
		return
	}
	deploymentID := strings.TrimSpace(r.URL.Query().Get("deployment_id"))
	if deploymentID != "" && !deploymentIDRe.MatchString(deploymentID) {
		writeErrorJSON(w, "invalid deployment_id", http.StatusBadRequest)
		return
	}

	all := s.audioStore.list(agentID, deploymentID)
	sessions := make([]audioSessionInfo, 0, len(all))
	for _, info := range all {
		ok, err := s.canAccessAgent(r.Context(), p, info.AgentID)
		if err != nil {
			writeErrorJSON(w, "db error", http.StatusInternalServerError)
			return
		}
		if ok {
			sessions = append(sessions, info)
		}
	}

	writeJSON(w, map[string]any{
		"ok":       true,
		"count":    len(sessions),
		"sessions": sessions,
	})
}

func (s *Server) handleAudioSessionCreate(w http.ResponseWriter, r *http.Request) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	body, err := readBodyBounded(r.Body, 64*1024)
	if err != nil {
		writeErrorJSON(w, "invalid body", http.StatusBadRequest)
		return
	}
	req := struct {
		AgentID      string `json:"agent_id"`
		DeploymentID string `json:"deployment_id,omitempty"`
		Mode         string `json:"mode,omitempty"`
		Metadata     any    `json:"metadata,omitempty"`
	}{}
	if len(body) > 0 {
		if err := json.Unmarshal(body, &req); err != nil {
			writeErrorJSON(w, "invalid json", http.StatusBadRequest)
			return
		}
	}
	agentID := strings.TrimSpace(req.AgentID)
	if agentID == "" || !agentIDRe.MatchString(agentID) {
		writeErrorJSON(w, "invalid agent_id", http.StatusBadRequest)
		return
	}
	if ok, err := s.canAccessAgent(r.Context(), p, agentID); err != nil {
		writeErrorJSON(w, "db error", http.StatusInternalServerError)
		return
	} else if !ok {
		writeErrorJSON(w, "forbidden", http.StatusForbidden)
		return
	}

	mode := strings.TrimSpace(req.Mode)
	if mode == "" {
		mode = "webrtc"
	}
	if mode != "webrtc" {
		writeErrorJSON(w, "unsupported mode", http.StatusBadRequest)
		return
	}
	deploymentID := strings.TrimSpace(req.DeploymentID)
	if deploymentID != "" && !deploymentIDRe.MatchString(deploymentID) {
		writeErrorJSON(w, "invalid deployment_id", http.StatusBadRequest)
		return
	}
	if deploymentID == "" {
		deploymentID = "default"
	}

	sess := s.audioStore.create(agentID, deploymentID, p.Sub, mode)
	if sess == nil {
		writeErrorJSON(w, "audio session unavailable", http.StatusInternalServerError)
		return
	}
	exp := sess.expiresAt
	if exp.IsZero() {
		exp = time.Now().Add(defaultAudioSessionTTL)
	}

	writeJSON(w, map[string]any{
		"ok":              true,
		"session_id":      sess.id,
		"expires_unix_ms": exp.UnixMilli(),
		"signal": map[string]any{
			"send_url": "/v1/audio/sessions/" + sess.id + "/signal",
			"recv_url": "/v1/audio/sessions/" + sess.id + "/signal/stream",
		},
	})
}

func (s *Server) handleAudioSessionGet(w http.ResponseWriter, r *http.Request, sessionID string) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if r.Method != "GET" {
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	sess, ok := s.audioStore.get(sessionID)
	if !ok || sess == nil {
		writeErrorJSON(w, "session not found", http.StatusNotFound)
		return
	}
	if ok, err := s.canAccessAgent(r.Context(), p, sess.agentID); err != nil {
		writeErrorJSON(w, "db error", http.StatusInternalServerError)
		return
	} else if !ok {
		writeErrorJSON(w, "forbidden", http.StatusForbidden)
		return
	}
	writeJSON(w, map[string]any{
		"ok":      true,
		"session": sess.snapshot(),
	})
}

func (s *Server) handleAudioSessionDelete(w http.ResponseWriter, r *http.Request, sessionID string) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if r.Method != "DELETE" {
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	sess, ok := s.audioStore.get(sessionID)
	if !ok || sess == nil {
		writeErrorJSON(w, "session not found", http.StatusNotFound)
		return
	}
	if ok, err := s.canAccessAgent(r.Context(), p, sess.agentID); err != nil {
		writeErrorJSON(w, "db error", http.StatusInternalServerError)
		return
	} else if !ok {
		writeErrorJSON(w, "forbidden", http.StatusForbidden)
		return
	}
	s.audioStore.delete(sessionID)
	writeJSON(w, map[string]any{
		"ok":         true,
		"deleted":    true,
		"session_id": sessionID,
	})
}

func (s *Server) handleAudioSignalSend(w http.ResponseWriter, r *http.Request, sessionID string) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if r.Method != "POST" {
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	sess, ok := s.audioStore.get(sessionID)
	if !ok || sess == nil {
		writeErrorJSON(w, "session not found", http.StatusNotFound)
		return
	}
	if ok, err := s.canAccessAgent(r.Context(), p, sess.agentID); err != nil {
		writeErrorJSON(w, "db error", http.StatusInternalServerError)
		return
	} else if !ok {
		writeErrorJSON(w, "forbidden", http.StatusForbidden)
		return
	}

	body, err := readBodyBounded(r.Body, 256*1024)
	if err != nil {
		writeErrorJSON(w, "invalid body", http.StatusBadRequest)
		return
	}
	var req struct {
		Type    string          `json:"type"`
		Payload json.RawMessage `json:"payload,omitempty"`
	}
	if len(body) == 0 {
		writeErrorJSON(w, "missing body", http.StatusBadRequest)
		return
	}
	if err := json.Unmarshal(body, &req); err != nil {
		writeErrorJSON(w, "invalid json", http.StatusBadRequest)
		return
	}
	msgType := strings.TrimSpace(req.Type)
	if msgType == "" {
		writeErrorJSON(w, "missing type", http.StatusBadRequest)
		return
	}
	if !isAudioSignalType(msgType) {
		writeErrorJSON(w, "unsupported type", http.StatusBadRequest)
		return
	}
	from := "webui"
	if p.AuthKind == "client" {
		from = "agentd"
	}
	ev := audioSignalEvent{
		Type:     msgType,
		Payload:  req.Payload,
		From:     from,
		TsUnixMs: time.Now().UnixMilli(),
	}
	sess.broadcast(ev)
	if msgType == "bye" {
		s.audioStore.delete(sessionID)
	}
	writeJSON(w, map[string]any{"ok": true})
}

func (s *Server) handleAudioSignalStream(w http.ResponseWriter, r *http.Request, sessionID string) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if r.Method != "GET" {
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	sess, ok := s.audioStore.get(sessionID)
	if !ok || sess == nil {
		writeErrorJSON(w, "session not found", http.StatusNotFound)
		return
	}
	if ok, err := s.canAccessAgent(r.Context(), p, sess.agentID); err != nil {
		writeErrorJSON(w, "db error", http.StatusInternalServerError)
		return
	} else if !ok {
		writeErrorJSON(w, "forbidden", http.StatusForbidden)
		return
	}
	fl, ok := w.(http.Flusher)
	if !ok {
		writeErrorJSON(w, "streaming unsupported", http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("Connection", "keep-alive")
	w.Header().Set("X-Accel-Buffering", "no")

	subID, ch := sess.subscribe()
	defer sess.unsubscribe(subID)

	if _, err := w.Write([]byte(":ok\n\n")); err != nil {
		return
	}
	fl.Flush()

	keepAlive := 15 * time.Second
	t := time.NewTicker(keepAlive)
	defer t.Stop()

	for {
		select {
		case <-r.Context().Done():
			return
		case <-t.C:
			if _, err := w.Write([]byte(":keepalive\n\n")); err != nil {
				return
			}
			fl.Flush()
		case ev, ok := <-ch:
			if !ok {
				return
			}
			b, _ := json.Marshal(ev)
			if _, err := w.Write([]byte("event: signal\n")); err != nil {
				return
			}
			if _, err := w.Write([]byte("data: ")); err != nil {
				return
			}
			if _, err := w.Write(b); err != nil {
				return
			}
			if _, err := w.Write([]byte("\n\n")); err != nil {
				return
			}
			fl.Flush()
		}
	}
}

func isAudioSignalType(t string) bool {
	switch t {
	case "offer", "answer", "candidate", "bye", "control":
		return true
	default:
		return false
	}
}
