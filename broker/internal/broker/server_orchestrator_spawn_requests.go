package broker

import (
	"encoding/json"
	"errors"
	"net/http"
	"strings"

	"agentd-broker/internal/db"
	"agentd-broker/internal/events"
)

func (s *Server) handleTeamOrchestratorSpawnRequestsList(w http.ResponseWriter, r *http.Request, teamID string) {
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
	limit := 25
	if v := strings.TrimSpace(r.URL.Query().Get("limit")); v != "" {
		if n, ok := parseIntBounded(v, 1, 200); ok {
			limit = n
		}
	}
	offset := 0
	if v := strings.TrimSpace(r.URL.Query().Get("offset")); v != "" {
		if n, ok := parseIntBounded(v, 0, 10_000); ok {
			offset = n
		}
	}
	status := strings.TrimSpace(r.URL.Query().Get("status"))
	if status != "" {
		status = strings.ToLower(status)
	}
	orchestratorRunID := strings.TrimSpace(r.URL.Query().Get("orchestrator_run_id"))
	rows, err := s.cfg.DB.ListAgentSpawnRequests(r.Context(), teamID, limit, offset, status, orchestratorRunID)
	if err != nil {
		writeErrorJSON(w, "db error", http.StatusInternalServerError)
		return
	}
	out := make([]map[string]any, 0, len(rows))
	for _, row := range rows {
		out = append(out, spawnRequestToJSON(row))
	}
	resp := map[string]any{
		"ok":             true,
		"team_id":        teamID,
		"limit":          limit,
		"offset":         offset,
		"spawn_requests": out,
	}
	if status != "" {
		resp["status"] = status
	}
	if orchestratorRunID != "" {
		resp["orchestrator_run_id"] = orchestratorRunID
	}
	writeJSON(w, resp)
}

func (s *Server) handleTeamOrchestratorSpawnRequestCreate(w http.ResponseWriter, r *http.Request, teamID string) {
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
	body, err := readBodyBounded(r.Body, 1024*1024)
	if err != nil {
		writeErrorJSON(w, "invalid body", http.StatusBadRequest)
		return
	}
	req := struct {
		SpawnRequestID    string           `json:"spawn_request_id"`
		OrchestratorRunID string           `json:"orchestrator_run_id"`
		Role              string           `json:"role"`
		Count             int              `json:"count"`
		Status            string           `json:"status"`
		Requirements      map[string]any   `json:"requirements"`
		AssignedMembers   []map[string]any `json:"assigned_members"`
		Meta              map[string]any   `json:"meta"`
	}{}
	if len(body) > 0 {
		if err := json.Unmarshal(body, &req); err != nil {
			writeErrorJSON(w, "invalid json", http.StatusBadRequest)
			return
		}
	}
	spawnID := strings.TrimSpace(req.SpawnRequestID)
	if spawnID == "" {
		spawnID = "ospawn_" + newID()[:12]
	}
	if !spawnRequestIDRe.MatchString(spawnID) {
		writeErrorJSON(w, "invalid spawn_request_id", http.StatusBadRequest)
		return
	}
	role := strings.ToLower(strings.TrimSpace(req.Role))
	if role == "" {
		writeErrorJSON(w, "missing role", http.StatusBadRequest)
		return
	}
	count := req.Count
	if count <= 0 {
		count = 1
	}
	status := strings.TrimSpace(req.Status)
	if status == "" {
		status = "requested"
	}
	orchestratorRunID := strings.TrimSpace(req.OrchestratorRunID)
	if orchestratorRunID != "" {
		if _, err := s.cfg.DB.GetOrchestratorRun(r.Context(), teamID, orchestratorRunID); err != nil {
			writeErrorJSON(w, "orchestrator run not found", http.StatusBadRequest)
			return
		}
	}
	spawn, err := s.cfg.DB.CreateAgentSpawnRequest(
		r.Context(),
		spawnID,
		teamID,
		orchestratorRunID,
		role,
		count,
		status,
		p.Sub,
		req.Requirements,
		req.AssignedMembers,
		req.Meta,
	)
	if err != nil {
		writeErrorJSON(w, "create spawn request failed", http.StatusBadRequest)
		return
	}
	publishOrchestratorSpawnRequested(s.cfg.Events, p.Sub, spawn, traceIDFromContext(r.Context()))
	writeJSON(w, map[string]any{"ok": true, "team_id": teamID, "spawn_request": spawnRequestToJSON(*spawn)})
}

func (s *Server) handleTeamOrchestratorSpawnRequestGet(w http.ResponseWriter, r *http.Request, teamID, spawnID string) {
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
	spawn, err := s.cfg.DB.GetAgentSpawnRequest(r.Context(), teamID, spawnID)
	if err != nil {
		writeErrorJSON(w, "spawn request not found", http.StatusNotFound)
		return
	}
	writeJSON(w, map[string]any{"ok": true, "team_id": teamID, "spawn_request": spawnRequestToJSON(*spawn)})
}

func (s *Server) handleTeamOrchestratorSpawnRequestUpdate(w http.ResponseWriter, r *http.Request, teamID, spawnID string) {
	if r.Method != "PATCH" {
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
	body, err := readBodyBounded(r.Body, 1024*1024)
	if err != nil {
		writeErrorJSON(w, "invalid body", http.StatusBadRequest)
		return
	}
	req := struct {
		Status          *string           `json:"status"`
		ExpectedStatus  *string           `json:"expected_status"`
		Requirements    map[string]any    `json:"requirements"`
		AssignedMembers *[]map[string]any `json:"assigned_members"`
		Error           *string           `json:"error"`
		Meta            map[string]any    `json:"meta"`
	}{}
	if len(body) > 0 {
		if err := json.Unmarshal(body, &req); err != nil {
			writeErrorJSON(w, "invalid json", http.StatusBadRequest)
			return
		}
	}
	prev, err := s.cfg.DB.GetAgentSpawnRequest(r.Context(), teamID, spawnID)
	if err != nil {
		writeErrorJSON(w, "spawn request not found", http.StatusNotFound)
		return
	}
	update := db.AgentSpawnRequestUpdate{
		Status:          req.Status,
		ExpectedStatus:  req.ExpectedStatus,
		Requirements:    req.Requirements,
		AssignedMembers: req.AssignedMembers,
		Error:           req.Error,
		Meta:            req.Meta,
	}
	spawn, err := s.cfg.DB.UpdateAgentSpawnRequest(r.Context(), teamID, spawnID, update)
	if err != nil {
		if errors.Is(err, db.ErrAgentSpawnRequestConflict) {
			writeErrorJSON(w, "spawn request status mismatch", http.StatusConflict)
			return
		}
		writeErrorJSON(w, err.Error(), http.StatusBadRequest)
		return
	}
	trackID := traceIDFromContext(r.Context())
	publishOrchestratorSpawnUpdated(s.cfg.Events, p.Sub, spawn, trackID)
	if req.Status != nil && strings.TrimSpace(*req.Status) != "" && spawn.Status != prev.Status {
		publishOrchestratorSpawnStatus(s.cfg.Events, p.Sub, spawn, trackID)
	}
	writeJSON(w, map[string]any{"ok": true, "team_id": teamID, "spawn_request": spawnRequestToJSON(*spawn)})
}

func spawnRequestToJSON(req db.AgentSpawnRequest) map[string]any {
	out := map[string]any{
		"spawn_request_id": req.SpawnRequestID,
		"team_id":          req.TeamID,
		"role":             req.Role,
		"count":            req.Count,
		"status":           req.Status,
		"requirements":     req.Requirements(),
		"assigned_members": req.AssignedMembers(),
		"created_by":       req.CreatedBy,
		"created_unix_ms":  req.CreatedAt.UnixMilli(),
		"updated_unix_ms":  req.UpdatedAt.UnixMilli(),
		"meta":             req.Meta(),
	}
	if req.OrchestratorRunID != "" {
		out["orchestrator_run_id"] = req.OrchestratorRunID
	}
	if strings.TrimSpace(req.ErrorText) != "" {
		out["error"] = req.ErrorText
	}
	return out
}

func publishOrchestratorSpawnRequested(hub *events.Hub, userSub string, req *db.AgentSpawnRequest, traceID string) {
	if hub == nil || userSub == "" || req == nil {
		return
	}
	payload := spawnRequestEventPayload(*req)
	ev := events.Event{
		Type:    "orchestrator_spawn_requested",
		UserSub: userSub,
		TraceID: traceID,
		Payload: payload,
	}
	hub.PublishTo([]string{userSub}, ev)
}

func publishOrchestratorSpawnUpdated(hub *events.Hub, userSub string, req *db.AgentSpawnRequest, traceID string) {
	if hub == nil || userSub == "" || req == nil {
		return
	}
	payload := spawnRequestEventPayload(*req)
	ev := events.Event{
		Type:    "orchestrator_spawn_updated",
		UserSub: userSub,
		TraceID: traceID,
		Payload: payload,
	}
	hub.PublishTo([]string{userSub}, ev)
}

func publishOrchestratorSpawnStatus(hub *events.Hub, userSub string, req *db.AgentSpawnRequest, traceID string) {
	if hub == nil || userSub == "" || req == nil {
		return
	}
	payload := spawnRequestEventPayload(*req)
	ev := events.Event{
		Type:    "orchestrator_spawn_status",
		UserSub: userSub,
		TraceID: traceID,
		Payload: payload,
	}
	hub.PublishTo([]string{userSub}, ev)
}

func spawnRequestEventPayload(req db.AgentSpawnRequest) map[string]any {
	payload := map[string]any{
		"team_id":          req.TeamID,
		"spawn_request_id": req.SpawnRequestID,
		"role":             req.Role,
		"count":            req.Count,
		"status":           req.Status,
		"requirements":     req.Requirements(),
		"assigned_members": req.AssignedMembers(),
		"created_unix_ms":  req.CreatedAt.UnixMilli(),
		"updated_unix_ms":  req.UpdatedAt.UnixMilli(),
	}
	if req.OrchestratorRunID != "" {
		payload["orchestrator_run_id"] = req.OrchestratorRunID
	}
	if strings.TrimSpace(req.ErrorText) != "" {
		payload["error"] = req.ErrorText
	}
	meta := req.Meta()
	if len(meta) > 0 {
		payload["meta"] = meta
	}
	return payload
}
