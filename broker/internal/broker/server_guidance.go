package broker

import (
	"encoding/json"
	"net/http"
	"strings"
	"time"

	"agentd-broker/internal/db"
	"agentd-broker/internal/events"
)

const guidanceMessageMaxBytes = 4096

var guidanceKinds = map[string]bool{
	"directive":  true,
	"context":    true,
	"warning":    true,
	"constraint": true,
}

var guidancePriorities = map[string]bool{
	"low":    true,
	"normal": true,
	"high":   true,
	"urgent": true,
}

func (s *Server) handleTeamGuidanceList(w http.ResponseWriter, r *http.Request, teamID string) {
	if r.Method != "GET" {
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if !s.allowAutomationPrincipal(p) {
		writeErrorJSON(w, "oidc required", http.StatusForbidden)
		return
	}
	if _, ok := s.requireTeamOwner(w, r, p, teamID); !ok {
		return
	}

	limit := 200
	if v := strings.TrimSpace(r.URL.Query().Get("limit")); v != "" {
		if n, ok := parseIntBounded(v, 1, 500); ok {
			limit = n
		}
	}
	offset := 0
	if v := strings.TrimSpace(r.URL.Query().Get("offset")); v != "" {
		if n, ok := parseIntBounded(v, 0, 10_000); ok {
			offset = n
		}
	}
	sinceTS := int64(0)
	if v := strings.TrimSpace(r.URL.Query().Get("since_ts")); v != "" {
		if n, ok := parseInt64Bounded(v, 0, 9_999_999_999_999); ok {
			sinceTS = n
		}
	}
	statuses := []string{}
	if v := strings.TrimSpace(r.URL.Query().Get("status")); v != "" {
		for _, part := range strings.Split(v, ",") {
			part = strings.ToLower(strings.TrimSpace(part))
			if part != "" {
				statuses = append(statuses, part)
			}
		}
	}
	teamRunID := strings.TrimSpace(r.URL.Query().Get("team_run_id"))

	rows, err := s.cfg.DB.ListGuidanceEvents(r.Context(), teamID, db.GuidanceListFilter{
		TeamRunID: teamRunID,
		SinceTS:   sinceTS,
		Limit:     limit,
		Offset:    offset,
		Statuses:  statuses,
	})
	if err != nil {
		writeErrorJSON(w, "db error", http.StatusInternalServerError)
		return
	}
	out := make([]map[string]any, 0, len(rows))
	for _, row := range rows {
		out = append(out, guidanceEventToJSON(row))
	}
	resp := map[string]any{
		"ok":       true,
		"team_id":  teamID,
		"limit":    limit,
		"offset":   offset,
		"since_ts": sinceTS,
		"count":    len(out),
		"guidance": out,
	}
	if teamRunID != "" {
		resp["team_run_id"] = teamRunID
	}
	if len(statuses) > 0 {
		resp["status"] = statuses
	}
	writeJSON(w, resp)
}

func (s *Server) handleTeamGuidanceCreate(w http.ResponseWriter, r *http.Request, teamID string) {
	if r.Method != "POST" {
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if p.AuthKind != "oidc" {
		writeErrorJSON(w, "oidc required", http.StatusForbidden)
		return
	}
	if _, ok := s.requireTeamOwner(w, r, p, teamID); !ok {
		return
	}
	body, err := readBodyBounded(r.Body, 1024*1024)
	if err != nil {
		writeErrorJSON(w, "invalid body", http.StatusBadRequest)
		return
	}
	req := struct {
		GuidanceID         string         `json:"guidance_id"`
		TeamRunID          string         `json:"team_run_id"`
		Kind               string         `json:"kind"`
		Priority           string         `json:"priority"`
		Message            string         `json:"message"`
		Payload            map[string]any `json:"payload"`
		TargetRoles        []string       `json:"target_roles"`
		TargetMemberIDs    []string       `json:"target_member_ids"`
		TargetAgentIDs     []string       `json:"target_agent_ids"`
		TargetOrchestrator string         `json:"target_orchestrator_id"`
		ExpiresUnixMS      int64          `json:"expires_unix_ms"`
		CreatedBy          string         `json:"created_by"`
	}{}
	if len(body) > 0 {
		if err := json.Unmarshal(body, &req); err != nil {
			writeErrorJSON(w, "invalid json", http.StatusBadRequest)
			return
		}
	}
	guidanceID := strings.TrimSpace(req.GuidanceID)
	if guidanceID == "" {
		guidanceID = "guidance_" + newID()[:12]
	}
	if !guidanceIDRe.MatchString(guidanceID) {
		writeErrorJSON(w, "invalid guidance_id", http.StatusBadRequest)
		return
	}
	kind := strings.ToLower(strings.TrimSpace(req.Kind))
	if kind == "" {
		writeErrorJSON(w, "missing kind", http.StatusBadRequest)
		return
	}
	if !guidanceKinds[kind] {
		writeErrorJSON(w, "invalid kind", http.StatusBadRequest)
		return
	}
	priority := strings.ToLower(strings.TrimSpace(req.Priority))
	if priority == "" {
		priority = "normal"
	}
	if !guidancePriorities[priority] {
		writeErrorJSON(w, "invalid priority", http.StatusBadRequest)
		return
	}
	message := strings.TrimSpace(req.Message)
	if message == "" {
		writeErrorJSON(w, "missing message", http.StatusBadRequest)
		return
	}
	if len(message) > guidanceMessageMaxBytes {
		writeErrorJSON(w, "message too long", http.StatusBadRequest)
		return
	}
	teamRunID := strings.TrimSpace(req.TeamRunID)
	if teamRunID != "" {
		if _, err := s.cfg.DB.GetTeamRun(r.Context(), teamID, teamRunID); err != nil {
			writeErrorJSON(w, "team run not found", http.StatusNotFound)
			return
		}
	}
	createdBy := strings.TrimSpace(req.CreatedBy)
	if createdBy == "" {
		createdBy = p.Sub
	}
	targetRoles := normalizeStringList(req.TargetRoles)
	targetMemberIDs := normalizeStringList(req.TargetMemberIDs)
	targetAgentIDs := normalizeStringList(req.TargetAgentIDs)
	targetOrchestratorID := strings.TrimSpace(req.TargetOrchestrator)
	expiresUnixMS := req.ExpiresUnixMS
	if expiresUnixMS < 0 {
		expiresUnixMS = 0
	}
	createdUnixMS := time.Now().UnixMilli()

	ev, err := s.cfg.DB.CreateGuidanceEvent(
		r.Context(),
		guidanceID,
		teamID,
		teamRunID,
		kind,
		priority,
		message,
		createdBy,
		p.Sub,
		createdUnixMS,
		expiresUnixMS,
		req.Payload,
		targetRoles,
		targetMemberIDs,
		targetAgentIDs,
		targetOrchestratorID,
	)
	if err != nil {
		writeErrorJSON(w, "create guidance failed", http.StatusBadRequest)
		return
	}
	traceID := traceIDFromContext(r.Context())
	publishTeamGuidanceCreated(s.cfg.Events, p.Sub, *ev, traceID)
	writeJSON(w, map[string]any{"ok": true, "team_id": teamID, "guidance": guidanceEventToJSON(*ev)})
}

func (s *Server) handleTeamGuidanceGet(w http.ResponseWriter, r *http.Request, teamID, guidanceID string) {
	if r.Method != "GET" {
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if !s.allowAutomationPrincipal(p) {
		writeErrorJSON(w, "oidc required", http.StatusForbidden)
		return
	}
	if _, ok := s.requireTeamOwner(w, r, p, teamID); !ok {
		return
	}
	guidanceID = strings.TrimSpace(guidanceID)
	if guidanceID == "" || !guidanceIDRe.MatchString(guidanceID) {
		writeErrorJSON(w, "invalid guidance_id", http.StatusBadRequest)
		return
	}
	ev, err := s.cfg.DB.GetGuidanceEvent(r.Context(), teamID, guidanceID)
	if err != nil {
		writeErrorJSON(w, "guidance not found", http.StatusNotFound)
		return
	}
	writeJSON(w, map[string]any{"ok": true, "team_id": teamID, "guidance": guidanceEventToJSON(*ev)})
}

func (s *Server) handleTeamGuidanceAck(w http.ResponseWriter, r *http.Request, teamID, guidanceID string) {
	if r.Method != "POST" {
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if !s.allowAutomationPrincipal(p) {
		writeErrorJSON(w, "oidc required", http.StatusForbidden)
		return
	}
	if _, ok := s.requireTeamOwner(w, r, p, teamID); !ok {
		return
	}
	guidanceID = strings.TrimSpace(guidanceID)
	if guidanceID == "" || !guidanceIDRe.MatchString(guidanceID) {
		writeErrorJSON(w, "invalid guidance_id", http.StatusBadRequest)
		return
	}
	body, err := readBodyBounded(r.Body, 1024*1024)
	if err != nil {
		writeErrorJSON(w, "invalid body", http.StatusBadRequest)
		return
	}
	req := struct {
		Status    string `json:"status"`
		Note      string `json:"note"`
		AckSource string `json:"ack_source"`
		AckRole   string `json:"ack_role"`
	}{}
	if len(body) > 0 {
		if err := json.Unmarshal(body, &req); err != nil {
			writeErrorJSON(w, "invalid json", http.StatusBadRequest)
			return
		}
	}
	status := strings.ToLower(strings.TrimSpace(req.Status))
	if status == "" {
		status = "acked"
	}
	if status != "acked" && status != "superseded" && status != "expired" {
		writeErrorJSON(w, "invalid status", http.StatusBadRequest)
		return
	}
	note := strings.TrimSpace(req.Note)
	if len(note) > guidanceMessageMaxBytes {
		writeErrorJSON(w, "note too long", http.StatusBadRequest)
		return
	}
	ackSource := strings.ToLower(strings.TrimSpace(req.AckSource))
	if ackSource == "" {
		if p.AuthKind == "client" {
			ackSource = "orchestrator"
		} else {
			ackSource = "human"
		}
	}
	ackRole := strings.TrimSpace(req.AckRole)
	ackUnixMS := time.Now().UnixMilli()

	ev, updated, err := s.cfg.DB.UpdateGuidanceAck(r.Context(), teamID, guidanceID, status, p.Sub, note, ackUnixMS)
	if err != nil {
		writeErrorJSON(w, "guidance not found", http.StatusNotFound)
		return
	}
	if !updated {
		writeErrorJSON(w, "guidance already closed", http.StatusConflict)
		return
	}
	receipt, err := s.cfg.DB.CreateGuidanceReceipt(r.Context(), guidanceID, p.Sub, ackRole, ackSource, note, ackUnixMS)
	if err != nil {
		writeErrorJSON(w, "receipt write failed", http.StatusInternalServerError)
		return
	}
	traceID := traceIDFromContext(r.Context())
	publishTeamGuidanceAck(s.cfg.Events, p.Sub, *ev, *receipt, traceID)
	writeJSON(w, map[string]any{
		"ok":          true,
		"team_id":     teamID,
		"guidance":    guidanceEventToJSON(*ev),
		"receipt":     guidanceReceiptToJSON(*receipt),
		"ack_unix_ms": ackUnixMS,
	})
}

func normalizeStringList(values []string) []string {
	if len(values) == 0 {
		return []string{}
	}
	seen := map[string]bool{}
	out := make([]string, 0, len(values))
	for _, v := range values {
		v = strings.TrimSpace(v)
		if v == "" || seen[v] {
			continue
		}
		seen[v] = true
		out = append(out, v)
	}
	return out
}

func guidanceEventToJSON(ev db.GuidanceEvent) map[string]any {
	out := map[string]any{
		"guidance_id":       ev.GuidanceID,
		"team_id":           ev.TeamID,
		"kind":              ev.Kind,
		"priority":          ev.Priority,
		"message":           ev.Message,
		"payload":           ev.Payload(),
		"target_roles":      ev.TargetRoles(),
		"target_member_ids": ev.TargetMemberIDs(),
		"target_agent_ids":  ev.TargetAgentIDs(),
		"created_by":        ev.CreatedBy,
		"created_sub":       ev.CreatedSub,
		"created_unix_ms":   ev.CreatedUnixMS,
		"expires_unix_ms":   ev.ExpiresUnixMS,
		"status":            ev.Status,
		"acked_by":          ev.AckedBy,
		"acked_unix_ms":     ev.AckedUnixMS,
		"ack_note":          ev.AckNote,
	}
	if ev.TeamRunID != "" {
		out["team_run_id"] = ev.TeamRunID
	}
	if ev.TargetOrchestrator != "" {
		out["target_orchestrator_id"] = ev.TargetOrchestrator
	}
	return out
}

func guidanceReceiptToJSON(r db.GuidanceReceipt) map[string]any {
	return map[string]any{
		"id":            r.ID,
		"guidance_id":   r.GuidanceID,
		"ack_by":        r.AckBy,
		"ack_role":      r.AckRole,
		"ack_source":    r.AckSource,
		"ack_note":      r.AckNote,
		"acked_unix_ms": r.AckedUnixMS,
	}
}

func publishTeamGuidanceCreated(hub *events.Hub, userSub string, ev db.GuidanceEvent, traceID string) {
	if hub == nil || userSub == "" {
		return
	}
	payload := guidanceEventToJSON(ev)
	hub.PublishTo([]string{userSub}, events.Event{
		Type:    "team_guidance_created",
		UserSub: userSub,
		TraceID: traceID,
		Payload: payload,
	})
}

func publishTeamGuidanceAck(hub *events.Hub, userSub string, ev db.GuidanceEvent, receipt db.GuidanceReceipt, traceID string) {
	if hub == nil || userSub == "" {
		return
	}
	payload := guidanceEventToJSON(ev)
	payload["receipt"] = guidanceReceiptToJSON(receipt)
	hub.PublishTo([]string{userSub}, events.Event{
		Type:    "team_guidance_ack",
		UserSub: userSub,
		TraceID: traceID,
		Payload: payload,
	})
}
