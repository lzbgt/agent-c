package broker

import (
	"encoding/json"
	"errors"
	"net/http"
	"reflect"
	"sort"
	"strings"
	"time"

	"agentd-broker/internal/db"
	"agentd-broker/internal/events"
)

const (
	maxOrchestratorGoalRevisions     = 50
	maxOrchestratorRolePlanRevisions = 50
)

type orchestratorRunUpdateInput struct {
	Status           *string        `json:"status"`
	ExpectedStatus   *string        `json:"expected_status"`
	ExpectedOwner    *string        `json:"expected_owner"`
	Goal             *string        `json:"goal"`
	GoalContract     map[string]any `json:"goal_contract"`
	RolePlanSnapshot map[string]any `json:"role_plan_snapshot"`
	Meta             map[string]any `json:"meta"`
}

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
	meta := initializeOrchestratorRevisionHistory(req.Meta, goal, req.GoalContract, rolePlan, p.Sub)
	run, err := s.cfg.DB.CreateOrchestratorRun(
		r.Context(),
		runID,
		teamID,
		status,
		goal,
		p.Sub,
		req.GoalContract,
		rolePlan,
		meta,
	)
	if err != nil {
		writeErrorJSON(w, "create orchestrator run failed", http.StatusBadRequest)
		return
	}
	traceID := traceIDFromContext(r.Context())
	publishOrchestratorRunCreated(s.cfg.Events, p.Sub, run, traceID)
	if run != nil {
		runMeta := run.Meta()
		if payload := buildGoalRevisionPayload(teamID, runID, runMeta); payload != nil {
			publishOrchestratorRevisionEvent(s.cfg.Events, p.Sub, "orchestrator_goal_revision", payload, traceID)
		}
		if payload := buildRolePlanRevisionPayload(teamID, runID, runMeta); payload != nil {
			publishOrchestratorRevisionEvent(s.cfg.Events, p.Sub, "orchestrator_role_plan_revision", payload, traceID)
		}
	}
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
	req := orchestratorRunUpdateInput{}
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
	prevMeta := prev.Meta()
	meta := map[string]any(nil)
	if req.Meta != nil {
		meta = cloneMap(req.Meta)
		mergeOrchestratorRevisionHistory(meta, prevMeta)
	}
	ensureMeta := func() map[string]any {
		if meta == nil {
			meta = cloneMap(prevMeta)
		}
		if meta == nil {
			meta = map[string]any{}
		}
		return meta
	}
	nowMs := time.Now().UTC().UnixMilli()
	metaChanged := false
	var goalRevisionPayload map[string]any
	var roleRevisionPayload map[string]any

	goalPrev := strings.TrimSpace(prev.Goal)
	goalNext := goalPrev
	if req.Goal != nil {
		goalNext = strings.TrimSpace(*req.Goal)
	}
	contractPrev := prev.GoalContract()
	contractNext := contractPrev
	if req.GoalContract != nil {
		contractNext = req.GoalContract
	}
	goalChanged := goalNext != goalPrev
	contractChanged := !reflect.DeepEqual(contractPrev, contractNext)
	if goalChanged || contractChanged {
		metaBase := ensureMeta()
		goalVersions := readRevisionEntries(metaBase, "goal_versions")
		if len(goalVersions) == 0 {
			goalVersions = readRevisionEntries(prevMeta, "goal_versions")
		}
		version := nextRevisionVersion(goalVersions)
		diff := mapDiffKeys(contractPrev, contractNext)
		entry := map[string]any{
			"version":         version,
			"updated_unix_ms": nowMs,
			"updated_by":      p.Sub,
			"goal":            goalNext,
			"goal_contract":   contractNext,
		}
		if goalChanged {
			entry["goal_changed"] = true
		}
		if contractChanged {
			entry["goal_contract_changed"] = true
		}
		if diff != nil {
			entry["goal_contract_diff"] = diff
		}
		goalVersions = append(goalVersions, entry)
		goalVersions = trimRevisionEntries(goalVersions, maxOrchestratorGoalRevisions)
		metaBase["goal_versions"] = goalVersions
		metaBase["goal_version"] = version
		metaBase["goal_updated_unix_ms"] = nowMs
		metaChanged = true
		goalRevisionPayload = map[string]any{
			"team_id":             teamID,
			"orchestrator_run_id": runID,
			"version":             version,
			"updated_unix_ms":     nowMs,
			"updated_by":          p.Sub,
			"goal":                goalNext,
			"goal_contract":       contractNext,
		}
		if goalChanged {
			goalRevisionPayload["goal_changed"] = true
			goalRevisionPayload["previous_goal"] = goalPrev
		}
		if contractChanged {
			goalRevisionPayload["goal_contract_changed"] = true
			goalRevisionPayload["previous_goal_contract"] = contractPrev
		}
		if diff != nil {
			goalRevisionPayload["goal_contract_diff"] = diff
		}
	}

	rolePrev := prev.RolePlanSnapshot()
	roleNext := rolePrev
	if req.RolePlanSnapshot != nil {
		roleNext = req.RolePlanSnapshot
	}
	roleChanged := !reflect.DeepEqual(rolePrev, roleNext)
	if roleChanged {
		metaBase := ensureMeta()
		roleVersions := readRevisionEntries(metaBase, "role_plan_versions")
		if len(roleVersions) == 0 {
			roleVersions = readRevisionEntries(prevMeta, "role_plan_versions")
		}
		version := nextRevisionVersion(roleVersions)
		diff := mapDiffKeys(rolePrev, roleNext)
		entry := map[string]any{
			"version":            version,
			"updated_unix_ms":    nowMs,
			"updated_by":         p.Sub,
			"role_plan_snapshot": roleNext,
		}
		if diff != nil {
			entry["role_plan_diff"] = diff
		}
		roleVersions = append(roleVersions, entry)
		roleVersions = trimRevisionEntries(roleVersions, maxOrchestratorRolePlanRevisions)
		metaBase["role_plan_versions"] = roleVersions
		metaBase["role_plan_version"] = version
		metaBase["role_plan_updated_unix_ms"] = nowMs
		metaChanged = true
		roleRevisionPayload = map[string]any{
			"team_id":                     teamID,
			"orchestrator_run_id":         runID,
			"version":                     version,
			"updated_unix_ms":             nowMs,
			"updated_by":                  p.Sub,
			"role_plan_snapshot":          roleNext,
			"previous_role_plan_snapshot": rolePrev,
		}
		if diff != nil {
			roleRevisionPayload["role_plan_diff"] = diff
		}
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
	if metaChanged || req.Meta != nil {
		update.Meta = meta
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
	if goalRevisionPayload != nil {
		publishOrchestratorRevisionEvent(s.cfg.Events, p.Sub, "orchestrator_goal_revision", goalRevisionPayload, trackID)
	}
	if roleRevisionPayload != nil {
		publishOrchestratorRevisionEvent(s.cfg.Events, p.Sub, "orchestrator_role_plan_revision", roleRevisionPayload, trackID)
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

func publishOrchestratorRevisionEvent(
	hub *events.Hub,
	userSub string,
	eventType string,
	payload map[string]any,
	traceID string,
) {
	if hub == nil || userSub == "" || eventType == "" || payload == nil {
		return
	}
	ev := events.Event{
		Type:    eventType,
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

func cloneMap(src map[string]any) map[string]any {
	if src == nil {
		return nil
	}
	out := make(map[string]any, len(src))
	for k, v := range src {
		out[k] = v
	}
	return out
}

func mergeOrchestratorRevisionHistory(dst, src map[string]any) {
	if dst == nil || src == nil {
		return
	}
	copyIfMissing := func(key string) {
		if _, ok := dst[key]; ok {
			return
		}
		if v, ok := src[key]; ok {
			dst[key] = v
		}
	}
	copyIfMissing("goal_versions")
	copyIfMissing("goal_version")
	copyIfMissing("goal_updated_unix_ms")
	copyIfMissing("role_plan_versions")
	copyIfMissing("role_plan_version")
	copyIfMissing("role_plan_updated_unix_ms")
}

func readRevisionEntries(meta map[string]any, key string) []map[string]any {
	if meta == nil {
		return nil
	}
	raw, ok := meta[key]
	if !ok || raw == nil {
		return nil
	}
	switch t := raw.(type) {
	case []map[string]any:
		out := make([]map[string]any, 0, len(t))
		for _, item := range t {
			if item != nil {
				out = append(out, item)
			}
		}
		return out
	case []any:
		out := make([]map[string]any, 0, len(t))
		for _, item := range t {
			if m, ok := item.(map[string]any); ok {
				out = append(out, m)
			}
		}
		return out
	default:
		return nil
	}
}

func nextRevisionVersion(entries []map[string]any) int64 {
	var max int64
	for _, entry := range entries {
		if entry == nil {
			continue
		}
		if v, ok := parseInt64Value(entry["version"]); ok && v > max {
			max = v
		}
	}
	return max + 1
}

func trimRevisionEntries(entries []map[string]any, max int) []map[string]any {
	if max <= 0 || len(entries) <= max {
		return entries
	}
	return entries[len(entries)-max:]
}

func initializeOrchestratorRevisionHistory(
	meta map[string]any,
	goal string,
	goalContract map[string]any,
	rolePlanSnapshot map[string]any,
	updatedBy string,
) map[string]any {
	if meta == nil {
		meta = map[string]any{}
	}
	nowMs := time.Now().UTC().UnixMilli()
	goalEntries := readRevisionEntries(meta, "goal_versions")
	if len(goalEntries) == 0 {
		entry := map[string]any{
			"version":         int64(1),
			"updated_unix_ms": nowMs,
			"updated_by":      updatedBy,
			"goal":            goal,
			"goal_contract":   goalContract,
		}
		meta["goal_versions"] = []map[string]any{entry}
		meta["goal_version"] = int64(1)
		meta["goal_updated_unix_ms"] = nowMs
	} else {
		if _, ok := meta["goal_version"]; !ok {
			meta["goal_version"] = nextRevisionVersion(goalEntries) - 1
		}
		if _, ok := meta["goal_updated_unix_ms"]; !ok {
			meta["goal_updated_unix_ms"] = nowMs
		}
	}

	roleEntries := readRevisionEntries(meta, "role_plan_versions")
	if len(roleEntries) == 0 && rolePlanSnapshot != nil {
		entry := map[string]any{
			"version":            int64(1),
			"updated_unix_ms":    nowMs,
			"updated_by":         updatedBy,
			"role_plan_snapshot": rolePlanSnapshot,
		}
		meta["role_plan_versions"] = []map[string]any{entry}
		meta["role_plan_version"] = int64(1)
		meta["role_plan_updated_unix_ms"] = nowMs
	} else if len(roleEntries) > 0 {
		if _, ok := meta["role_plan_version"]; !ok {
			meta["role_plan_version"] = nextRevisionVersion(roleEntries) - 1
		}
		if _, ok := meta["role_plan_updated_unix_ms"]; !ok {
			meta["role_plan_updated_unix_ms"] = nowMs
		}
	}
	return meta
}

func buildGoalRevisionPayload(teamID, runID string, meta map[string]any) map[string]any {
	entries := readRevisionEntries(meta, "goal_versions")
	if len(entries) == 0 {
		return nil
	}
	entry := entries[len(entries)-1]
	payload := map[string]any{
		"team_id":             teamID,
		"orchestrator_run_id": runID,
	}
	if v, ok := entry["version"]; ok {
		payload["version"] = v
	}
	if v, ok := entry["updated_unix_ms"]; ok {
		payload["updated_unix_ms"] = v
	}
	if v, ok := entry["updated_by"]; ok {
		payload["updated_by"] = v
	}
	if v, ok := entry["goal"]; ok {
		payload["goal"] = v
	}
	if v, ok := entry["goal_contract"]; ok {
		payload["goal_contract"] = v
	}
	if v, ok := entry["goal_contract_diff"]; ok {
		payload["goal_contract_diff"] = v
	}
	return payload
}

func buildRolePlanRevisionPayload(teamID, runID string, meta map[string]any) map[string]any {
	entries := readRevisionEntries(meta, "role_plan_versions")
	if len(entries) == 0 {
		return nil
	}
	entry := entries[len(entries)-1]
	payload := map[string]any{
		"team_id":             teamID,
		"orchestrator_run_id": runID,
	}
	if v, ok := entry["version"]; ok {
		payload["version"] = v
	}
	if v, ok := entry["updated_unix_ms"]; ok {
		payload["updated_unix_ms"] = v
	}
	if v, ok := entry["updated_by"]; ok {
		payload["updated_by"] = v
	}
	if v, ok := entry["role_plan_snapshot"]; ok {
		payload["role_plan_snapshot"] = v
	}
	if v, ok := entry["role_plan_diff"]; ok {
		payload["role_plan_diff"] = v
	}
	return payload
}

func mapDiffKeys(prev, next map[string]any) map[string]any {
	if len(prev) == 0 && len(next) == 0 {
		return nil
	}
	added := []string{}
	removed := []string{}
	changed := []string{}
	for key := range prev {
		if _, ok := next[key]; !ok {
			removed = append(removed, key)
		}
	}
	for key, val := range next {
		prevVal, ok := prev[key]
		if !ok {
			added = append(added, key)
			continue
		}
		if !reflect.DeepEqual(prevVal, val) {
			changed = append(changed, key)
		}
	}
	if len(added) == 0 && len(removed) == 0 && len(changed) == 0 {
		return nil
	}
	sort.Strings(added)
	sort.Strings(removed)
	sort.Strings(changed)
	out := map[string]any{}
	if len(added) > 0 {
		out["added"] = added
	}
	if len(removed) > 0 {
		out["removed"] = removed
	}
	if len(changed) > 0 {
		out["changed"] = changed
	}
	return out
}
