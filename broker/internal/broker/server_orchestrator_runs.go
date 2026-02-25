package broker

import (
	"encoding/json"
	"errors"
	"net/http"
	"strings"
	"time"

	"agentd-broker/internal/db"
	"agentd-broker/internal/events"
)

func (s *Server) handleTeamOrchestratorRunsList(w http.ResponseWriter, r *http.Request, teamID string) {
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
	rows, err := s.cfg.DB.ListOrchestratorRuns(r.Context(), teamID, limit, offset, status)
	if err != nil {
		writeErrorJSON(w, "db error", http.StatusInternalServerError)
		return
	}
	out := make([]map[string]any, 0, len(rows))
	for _, row := range rows {
		out = append(out, orchestratorRunToJSON(row))
	}
	resp := map[string]any{
		"ok":      true,
		"team_id": teamID,
		"limit":   limit,
		"offset":  offset,
		"runs":    out,
	}
	if status != "" {
		resp["status"] = status
	}
	writeJSON(w, resp)
}

func (s *Server) handleTeamOrchestratorRunCreate(w http.ResponseWriter, r *http.Request, teamID string) {
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
	team, ok := s.requireTeamOwner(w, r, p, teamID)
	if !ok {
		return
	}
	body, err := readBodyBounded(r.Body, 1024*1024)
	if err != nil {
		writeErrorJSON(w, "invalid body", http.StatusBadRequest)
		return
	}
	req := struct {
		OrchestratorRunID string         `json:"orchestrator_run_id"`
		Goal              string         `json:"goal"`
		Status            string         `json:"status"`
		GoalContract      map[string]any `json:"goal_contract"`
		RolePlanSnapshot  map[string]any `json:"role_plan_snapshot"`
		Meta              map[string]any `json:"meta"`
	}{}
	if len(body) > 0 {
		if err := json.Unmarshal(body, &req); err != nil {
			writeErrorJSON(w, "invalid json", http.StatusBadRequest)
			return
		}
	}
	runID := strings.TrimSpace(req.OrchestratorRunID)
	if runID == "" {
		runID = "orun_" + newID()[:12]
	}
	if !orchestratorRunIDRe.MatchString(runID) {
		writeErrorJSON(w, "invalid orchestrator_run_id", http.StatusBadRequest)
		return
	}
	status := strings.TrimSpace(req.Status)
	if status == "" {
		status = "running"
	}
	goal := strings.TrimSpace(req.Goal)
	if goal == "" {
		writeErrorJSON(w, "missing goal", http.StatusBadRequest)
		return
	}
	rolePlan := req.RolePlanSnapshot
	if rolePlan == nil {
		rolePlan = buildRolePlanSnapshot(team.Meta())
	}
	run, err := s.cfg.DB.CreateOrchestratorRun(
		r.Context(),
		runID,
		teamID,
		status,
		goal,
		p.Sub,
		req.GoalContract,
		rolePlan,
		req.Meta,
	)
	if err != nil {
		writeErrorJSON(w, "create orchestrator run failed", http.StatusBadRequest)
		return
	}
	publishOrchestratorRunCreated(s.cfg.Events, p.Sub, run, traceIDFromContext(r.Context()))
	writeJSON(w, map[string]any{"ok": true, "team_id": teamID, "run": orchestratorRunToJSON(*run)})
}

func (s *Server) handleTeamOrchestratorRunGet(w http.ResponseWriter, r *http.Request, teamID, runID string) {
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
	run, err := s.cfg.DB.GetOrchestratorRun(r.Context(), teamID, runID)
	if err != nil {
		writeErrorJSON(w, "orchestrator run not found", http.StatusNotFound)
		return
	}
	writeJSON(w, map[string]any{"ok": true, "team_id": teamID, "run": orchestratorRunToJSON(*run)})
}

func (s *Server) handleTeamOrchestratorRunUpdate(w http.ResponseWriter, r *http.Request, teamID, runID string) {
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
		Status           *string        `json:"status"`
		ExpectedStatus   *string        `json:"expected_status"`
		ExpectedOwner    *string        `json:"expected_owner"`
		Goal             *string        `json:"goal"`
		GoalContract     map[string]any `json:"goal_contract"`
		RolePlanSnapshot map[string]any `json:"role_plan_snapshot"`
		Meta             map[string]any `json:"meta"`
	}{}
	if len(body) > 0 {
		if err := json.Unmarshal(body, &req); err != nil {
			writeErrorJSON(w, "invalid json", http.StatusBadRequest)
			return
		}
	}
	prev, err := s.cfg.DB.GetOrchestratorRun(r.Context(), teamID, runID)
	if err != nil {
		writeErrorJSON(w, "orchestrator run not found", http.StatusNotFound)
		return
	}
	update := db.OrchestratorRunUpdate{
		Status:           req.Status,
		ExpectedStatus:   req.ExpectedStatus,
		ExpectedOwner:    req.ExpectedOwner,
		Goal:             req.Goal,
		GoalContract:     req.GoalContract,
		RolePlanSnapshot: req.RolePlanSnapshot,
		Meta:             req.Meta,
	}
	run, err := s.cfg.DB.UpdateOrchestratorRun(r.Context(), teamID, runID, update)
	if err != nil {
		if errors.Is(err, db.ErrOrchestratorRunConflict) {
			writeErrorJSON(w, "orchestrator run conflict", http.StatusConflict)
			return
		}
		writeErrorJSON(w, err.Error(), http.StatusBadRequest)
		return
	}
	trackID := traceIDFromContext(r.Context())
	publishOrchestratorRunUpdated(s.cfg.Events, p.Sub, run, trackID)
	if req.Status != nil && strings.TrimSpace(*req.Status) != "" && run.Status != prev.Status {
		publishOrchestratorRunStatus(s.cfg.Events, p.Sub, run, trackID)
	}
	writeJSON(w, map[string]any{"ok": true, "team_id": teamID, "run": orchestratorRunToJSON(*run)})
}

func (s *Server) handleTeamOrchestratorRunHeartbeat(w http.ResponseWriter, r *http.Request, teamID, runID string) {
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
	body, err := readBodyBounded(r.Body, 256*1024)
	if err != nil {
		writeErrorJSON(w, "invalid body", http.StatusBadRequest)
		return
	}
	req := struct {
		Status         *string `json:"status"`
		ExpectedStatus *string `json:"expected_status"`
		ExpectedOwner  *string `json:"expected_owner"`
	}{}
	if len(body) > 0 {
		if err := json.Unmarshal(body, &req); err != nil {
			writeErrorJSON(w, "invalid json", http.StatusBadRequest)
			return
		}
	}
	run, err := s.cfg.DB.UpdateOrchestratorRunHeartbeat(r.Context(), teamID, runID, req.Status, req.ExpectedOwner, req.ExpectedStatus)
	if err != nil {
		if errors.Is(err, db.ErrOrchestratorRunConflict) {
			writeErrorJSON(w, "orchestrator run conflict", http.StatusConflict)
			return
		}
		writeErrorJSON(w, err.Error(), http.StatusBadRequest)
		return
	}
	trackID := traceIDFromContext(r.Context())
	publishOrchestratorRunHeartbeat(s.cfg.Events, p.Sub, run, trackID)
	writeJSON(w, map[string]any{"ok": true, "team_id": teamID, "run": orchestratorRunToJSON(*run)})
}

func orchestratorRunToJSON(run db.OrchestratorRun) map[string]any {
	now := time.Now()
	leaseTimeoutMs, heartbeatAgeMs, leaseStatus := deriveOrchestratorLease(run, now)
	out := map[string]any{
		"orchestrator_run_id": run.OrchestratorRunID,
		"team_id":             run.TeamID,
		"status":              run.Status,
		"goal":                run.Goal,
		"created_by":          run.CreatedBy,
		"created_unix_ms":     run.CreatedAt.UnixMilli(),
		"updated_unix_ms":     run.UpdatedAt.UnixMilli(),
		"goal_contract":       run.GoalContract(),
		"role_plan_snapshot":  run.RolePlanSnapshot(),
		"meta":                run.Meta(),
	}
	if leaseTimeoutMs > 0 {
		out["lease_timeout_ms"] = leaseTimeoutMs
	}
	if run.LastHeartbeatAt != nil {
		out["last_heartbeat_unix_ms"] = run.LastHeartbeatAt.UnixMilli()
		if heartbeatAgeMs >= 0 {
			out["heartbeat_age_ms"] = heartbeatAgeMs
		}
	}
	if leaseStatus != "" {
		out["lease_status"] = leaseStatus
	}
	return out
}

func buildRolePlanSnapshot(teamMeta map[string]any) map[string]any {
	if teamMeta == nil {
		return nil
	}
	out := map[string]any{}
	if v, ok := teamMeta["role_graph"]; ok && v != nil {
		out["role_graph"] = v
	}
	if v, ok := teamMeta["role_instructions"]; ok && v != nil {
		out["role_instructions"] = v
	}
	if v, ok := teamMeta["role_prompt_mode"]; ok && v != nil {
		out["role_prompt_mode"] = v
	}
	if len(out) == 0 {
		return nil
	}
	return out
}

func publishOrchestratorRunCreated(hub *events.Hub, userSub string, run *db.OrchestratorRun, traceID string) {
	if hub == nil || userSub == "" || run == nil {
		return
	}
	payload := orchestratorRunEventPayload(*run)
	ev := events.Event{
		Type:    "orchestrator_run_created",
		UserSub: userSub,
		TraceID: traceID,
		Payload: payload,
	}
	hub.PublishTo([]string{userSub}, ev)
}

func publishOrchestratorRunUpdated(hub *events.Hub, userSub string, run *db.OrchestratorRun, traceID string) {
	if hub == nil || userSub == "" || run == nil {
		return
	}
	payload := orchestratorRunEventPayload(*run)
	ev := events.Event{
		Type:    "orchestrator_run_updated",
		UserSub: userSub,
		TraceID: traceID,
		Payload: payload,
	}
	hub.PublishTo([]string{userSub}, ev)
}

func publishOrchestratorRunStatus(hub *events.Hub, userSub string, run *db.OrchestratorRun, traceID string) {
	if hub == nil || userSub == "" || run == nil {
		return
	}
	payload := orchestratorRunEventPayload(*run)
	ev := events.Event{
		Type:    "orchestrator_run_status",
		UserSub: userSub,
		TraceID: traceID,
		Payload: payload,
	}
	hub.PublishTo([]string{userSub}, ev)
}

func publishOrchestratorRunHeartbeat(hub *events.Hub, userSub string, run *db.OrchestratorRun, traceID string) {
	if hub == nil || userSub == "" || run == nil {
		return
	}
	payload := orchestratorRunEventPayload(*run)
	ev := events.Event{
		Type:    "orchestrator_run_heartbeat",
		UserSub: userSub,
		TraceID: traceID,
		Payload: payload,
	}
	hub.PublishTo([]string{userSub}, ev)
}

func orchestratorRunEventPayload(run db.OrchestratorRun) map[string]any {
	now := time.Now()
	leaseTimeoutMs, heartbeatAgeMs, leaseStatus := deriveOrchestratorLease(run, now)
	payload := map[string]any{
		"team_id":             run.TeamID,
		"orchestrator_run_id": run.OrchestratorRunID,
		"status":              run.Status,
		"goal":                run.Goal,
		"created_unix_ms":     run.CreatedAt.UnixMilli(),
		"updated_unix_ms":     run.UpdatedAt.UnixMilli(),
	}
	if run.LastHeartbeatAt != nil {
		payload["last_heartbeat_unix_ms"] = run.LastHeartbeatAt.UnixMilli()
		if heartbeatAgeMs >= 0 {
			payload["heartbeat_age_ms"] = heartbeatAgeMs
		}
	}
	if leaseTimeoutMs > 0 {
		payload["lease_timeout_ms"] = leaseTimeoutMs
	}
	if leaseStatus != "" {
		payload["lease_status"] = leaseStatus
	}
	return payload
}

func deriveOrchestratorLease(run db.OrchestratorRun, now time.Time) (int64, int64, string) {
	meta := run.Meta()
	leaseTimeoutMs := int64(120000)
	if meta != nil {
		if v, ok := meta["lease_timeout_ms"]; ok {
			if n, ok := parseInt64Value(v); ok && n > 0 {
				leaseTimeoutMs = n
			}
		} else if v, ok := meta["heartbeat_timeout_ms"]; ok {
			if n, ok := parseInt64Value(v); ok && n > 0 {
				leaseTimeoutMs = n
			}
		}
	}
	if run.LastHeartbeatAt == nil {
		return leaseTimeoutMs, -1, "missing"
	}
	ageMs := now.Sub(*run.LastHeartbeatAt).Milliseconds()
	if leaseTimeoutMs <= 0 {
		return leaseTimeoutMs, ageMs, "unknown"
	}
	if ageMs > leaseTimeoutMs {
		return leaseTimeoutMs, ageMs, "stale"
	}
	return leaseTimeoutMs, ageMs, "ok"
}

func parseInt64Value(v any) (int64, bool) {
	switch t := v.(type) {
	case int:
		return int64(t), true
	case int64:
		return t, true
	case float64:
		return int64(t), true
	case json.Number:
		if n, err := t.Int64(); err == nil {
			return n, true
		}
	case string:
		if n, ok := parseInt64Bounded(t, 0, 9_999_999_999_999); ok {
			return n, true
		}
	}
	return 0, false
}
