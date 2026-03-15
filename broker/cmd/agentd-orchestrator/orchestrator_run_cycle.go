package main

import (
	"context"
	"fmt"
	"net/http"
	"os"
	"sort"
	"strings"
	"time"
)

func shouldAutonomous(meta map[string]any) bool {
	if meta == nil {
		return true
	}
	if v, ok := meta["autonomous"]; ok {
		if b, ok := asBool(v); ok {
			return b
		}
	}
	return true
}

func allowLeaseTakeover(meta map[string]any) bool {
	if meta == nil {
		return true
	}
	if v, ok := meta["allow_takeover"]; ok {
		if b, ok := asBool(v); ok {
			return b
		}
	}
	return true
}

func leaseStatusForRun(run orchestratorRun, meta map[string]any) string {
	if run.LeaseStatus != "" {
		return strings.ToLower(strings.TrimSpace(run.LeaseStatus))
	}
	timeoutMs := leaseTimeoutForRun(run, meta)
	if run.LastHeartbeatUnix == nil {
		if timeoutMs > 0 {
			return "missing"
		}
		return ""
	}
	if timeoutMs <= 0 {
		return "unknown"
	}
	age := time.Now().UTC().UnixMilli() - *run.LastHeartbeatUnix
	if age > timeoutMs {
		return "stale"
	}
	return "ok"
}

func leaseTimeoutForRun(run orchestratorRun, meta map[string]any) int64 {
	if run.LeaseTimeoutMS != nil && *run.LeaseTimeoutMS > 0 {
		return *run.LeaseTimeoutMS
	}
	if meta == nil {
		return 0
	}
	if v, ok := meta["lease_timeout_ms"]; ok {
		if n, ok := asInt64(v); ok && n > 0 {
			return n
		}
	} else if v, ok := meta["heartbeat_timeout_ms"]; ok {
		if n, ok := asInt64(v); ok && n > 0 {
			return n
		}
	}
	return 0
}

func handleRun(ctx context.Context, client *http.Client, cfg config, teamID string, run orchestratorRun) error {
	status := strings.ToLower(strings.TrimSpace(run.Status))
	if status == "" {
		status = "running"
	}
	meta := cloneMap(run.Meta)
	if _, ok := meta["orchestrator_id"]; !ok {
		meta["orchestrator_id"] = cfg.orchestratorID
	}
	owner := strings.TrimSpace(asString(meta["orchestrator_owner"]))
	leaseStatus := leaseStatusForRun(run, meta)
	if owner != "" && owner != cfg.orchestratorID {
		if leaseStatus == "" {
			return nil
		}
		if leaseStatus != "stale" && leaseStatus != "missing" {
			return nil
		}
		if !allowLeaseTakeover(meta) {
			return nil
		}
		meta["orchestrator_owner_prev"] = owner
		meta["orchestrator_owner"] = cfg.orchestratorID
		meta["orchestrator_owner_claimed_unix_ms"] = time.Now().UTC().UnixMilli()
		if _, err := updateOrchestratorRun(ctx, client, cfg, teamID, run.OrchestratorRunID, "", meta, stringPtr(owner), nil); err != nil {
			if isHTTPStatus(err, http.StatusConflict) {
				return nil
			}
			return err
		}
		owner = cfg.orchestratorID
	}
	if owner == "" {
		meta["orchestrator_owner"] = cfg.orchestratorID
		meta["orchestrator_owner_claimed_unix_ms"] = time.Now().UTC().UnixMilli()
		if _, err := updateOrchestratorRun(ctx, client, cfg, teamID, run.OrchestratorRunID, "", meta, stringPtr(""), nil); err != nil {
			if isHTTPStatus(err, http.StatusConflict) {
				return nil
			}
			return err
		}
		owner = cfg.orchestratorID
	}

	if status != "done" && status != "error" {
		hbStatus := status
		if hbStatus == "planned" {
			hbStatus = "running"
		}
		if hbStatus != "" {
			if hb, err := heartbeatRun(ctx, client, cfg, teamID, run.OrchestratorRunID, hbStatus, stringPtr(owner), nil); err == nil {
				if hb.Run.OrchestratorRunID != "" {
					run = hb.Run
					status = strings.ToLower(strings.TrimSpace(run.Status))
					meta = cloneMap(run.Meta)
					if _, ok := meta["orchestrator_id"]; !ok {
						meta["orchestrator_id"] = cfg.orchestratorID
					}
					owner = strings.TrimSpace(asString(meta["orchestrator_owner"]))
				}
			} else {
				if isHTTPStatus(err, http.StatusConflict) {
					return nil
				}
				fmt.Fprintf(os.Stderr, "heartbeat %s failed: %v\n", run.OrchestratorRunID, err)
			}
		}
	}

	activeRunID := strings.TrimSpace(asString(meta["active_team_run_id"]))
	guidanceChanged := false
	if owner == cfg.orchestratorID {
		if changed, err := processGuidance(ctx, client, cfg, teamID, activeRunID, meta); err != nil {
			return err
		} else if changed {
			guidanceChanged = true
		}
	}
	if activeRunID != "" {
		if status == "paused" || status == "waiting" {
			if owner == cfg.orchestratorID {
				if updated, err := maybeHandleReplanAck(ctx, client, cfg, teamID, run.OrchestratorRunID, owner, run, meta); err != nil {
					return err
				} else if updated {
					return nil
				}
			}
			if guidanceChanged {
				_, _ = updateOrchestratorRun(ctx, client, cfg, teamID, run.OrchestratorRunID, "", meta, stringPtr(owner), nil)
			}
			return nil
		}
		tr, err := fetchTeamRunStatus(ctx, client, cfg, teamID, activeRunID)
		if err != nil {
			return err
		}
		trStatus := strings.ToLower(strings.TrimSpace(tr.Status))
		if isTerminalTeamRunStatus(trStatus) {
			meta = appendTeamRunHistory(meta, activeRunID, trStatus)
			meta["last_team_run_status"] = trStatus
			if changed, err := maybeRetireRuntimeMembers(ctx, client, cfg, teamID, activeRunID, tr, meta); err != nil {
				fmt.Fprintf(os.Stderr, "runtime member retire failed: %v\n", err)
			} else if changed {
				// metadata updated by retire helper
			}
			completionMode := strings.ToLower(strings.TrimSpace(asString(meta["completion_mode"])))
			if completionMode == "" {
				completionMode = "on_success"
			}
			nextStatus := ""
			if completionMode != "never" {
				if trStatus == "succeeded" {
					nextStatus = "done"
				} else {
					nextStatus = "error"
				}
			}
			_, err := updateOrchestratorRun(ctx, client, cfg, teamID, run.OrchestratorRunID, nextStatus, meta, stringPtr(owner), nil)
			return err
		}
		metaChanged := guidanceChanged
		changed, err := tickActiveTeamRun(ctx, client, cfg, teamID, activeRunID, run.OrchestratorRunID, owner, run, tr, meta)
		if err != nil {
			return err
		}
		if changed {
			metaChanged = true
		}
		missingRoles := normalizeRoles(asStringSlice(tr.AutoAllocateMissing))
		missingForSpawn := missingRoles
		if shouldAutoAllocateRuntimeMembers(meta, tr) && len(missingRoles) > 0 {
			missingForSpawn = resolveAllocatorMissing(meta, activeRunID, missingRoles)
			if ok, after, err := maybeAllocateRuntimeMembers(ctx, client, cfg, teamID, activeRunID, tr, meta, missingRoles); err != nil {
				return err
			} else if ok {
				metaChanged = true
				missingForSpawn = after
			}
		}
		if ok, err := maybeCapacityAutoscaleRuntime(ctx, client, cfg, teamID, activeRunID, run.OrchestratorRunID, owner, run, tr, meta); err != nil {
			return err
		} else if ok {
			metaChanged = true
		}
		if shouldSpawnMissingRoles(meta) {
			if err := ensureSpawnRequests(ctx, client, cfg, teamID, run.OrchestratorRunID, activeRunID, meta, missingForSpawn, owner); err == nil {
				metaChanged = true
			}
		}
		if metaChanged {
			_, _ = updateOrchestratorRun(ctx, client, cfg, teamID, run.OrchestratorRunID, "", meta, stringPtr(owner), nil)
		}
		return nil
	}

	if status == "done" || status == "error" || status == "paused" || status == "waiting" {
		if guidanceChanged {
			_, _ = updateOrchestratorRun(ctx, client, cfg, teamID, run.OrchestratorRunID, "", meta, stringPtr(owner), nil)
		}
		return nil
	}

	runPayload, teamMeta := buildTeamRunPayload(run, meta)
	created, err := createTeamRun(ctx, client, cfg, teamID, runPayload, teamMeta)
	if err != nil {
		if isNoEligibleMembers(err) {
			roles := collectRolesFromPlan(run, meta)
			if len(roles) > 0 && shouldSpawnMissingRoles(meta) {
				_ = ensureSpawnRequests(ctx, client, cfg, teamID, run.OrchestratorRunID, "", meta, roles, owner)
			}
			return nil
		}
		return err
	}
	activeRunID = created.TeamRunID
	if activeRunID == "" {
		return nil
	}
	meta["active_team_run_id"] = activeRunID
	meta["last_team_run_status"] = created.Status
	if _, err := updateOrchestratorRun(ctx, client, cfg, teamID, run.OrchestratorRunID, "", meta, stringPtr(owner), nil); err != nil {
		return err
	}
	tr, err := fetchTeamRunStatus(ctx, client, cfg, teamID, activeRunID)
	if err == nil && shouldSpawnMissingRoles(meta) {
		_ = ensureSpawnRequests(ctx, client, cfg, teamID, run.OrchestratorRunID, activeRunID, meta, asStringSlice(tr.AutoAllocateMissing), owner)
	}
	return nil
}

func tickActiveTeamRun(
	ctx context.Context,
	client *http.Client,
	cfg config,
	teamID,
	teamRunID,
	orchestratorRunID,
	owner string,
	run orchestratorRun,
	status *teamRunResponse,
	meta map[string]any,
) (bool, error) {
	if status == nil {
		return false, nil
	}
	changed := false
	if ok, err := maybeEmitProgress(ctx, client, cfg, teamID, teamRunID, status, meta); err != nil {
		return changed, err
	} else if ok {
		changed = true
	}
	if ok, err := maybeEmitDrift(ctx, client, cfg, teamID, teamRunID, orchestratorRunID, owner, run, status, meta); err != nil {
		return changed, err
	} else if ok {
		changed = true
	}
	if ok, err := maybeProcessHandoffQueue(ctx, client, cfg, teamID, teamRunID, meta); err != nil {
		return changed, err
	} else if ok {
		changed = true
	}
	return changed, nil
}

func buildTeamRunPayload(run orchestratorRun, meta map[string]any) (map[string]any, map[string]any) {
	runTemplate := map[string]any{}
	if raw, ok := meta["run_template"].(map[string]any); ok {
		for k, v := range raw {
			runTemplate[k] = v
		}
	}
	prompt := strings.TrimSpace(asString(runTemplate["prompt"]))
	if prompt == "" {
		prompt = strings.TrimSpace(run.Goal)
	}
	if prompt != "" {
		runTemplate["prompt"] = prompt
	}
	teamMeta := map[string]any{}
	if run.RolePlanSnapshot != nil {
		for _, key := range []string{"role_graph", "role_instructions", "role_prompt_mode"} {
			if v, ok := run.RolePlanSnapshot[key]; ok && v != nil {
				teamMeta[key] = v
			}
		}
	}
	if run.GoalContract != nil {
		teamMeta["goal_contract"] = run.GoalContract
	}
	teamMode := strings.TrimSpace(asString(meta["team_mode"]))
	if teamMode == "" {
		teamMode = "async"
	}
	teamMeta["mode"] = teamMode
	if v, ok := meta["auto_allocate_roles"]; ok {
		if b, ok := asBool(v); ok {
			teamMeta["auto_allocate_roles"] = b
		}
	} else {
		teamMeta["auto_allocate_roles"] = true
	}
	if n, ok := asInt(meta["auto_allocate_max_members"]); ok && n > 0 {
		teamMeta["auto_allocate_max_members"] = n
	}
	if v, ok := meta["shared_memory_scope_id"]; ok && v != nil {
		teamMeta["shared_memory_scope_id"] = v
	}
	if v, ok := meta["shared_memory_mode"]; ok && v != nil {
		teamMeta["shared_memory_mode"] = v
	}
	if overrides, ok := meta["team_overrides"].(map[string]any); ok {
		for k, v := range overrides {
			teamMeta[k] = v
		}
	}
	return runTemplate, teamMeta
}

func collectRolesFromPlan(run orchestratorRun, meta map[string]any) []string {
	roles := map[string]bool{}
	if desired := asStringSlice(meta["desired_roles"]); len(desired) > 0 {
		for _, r := range desired {
			if r != "" {
				roles[strings.ToLower(strings.TrimSpace(r))] = true
			}
		}
	}
	if run.RolePlanSnapshot != nil {
		if raw, ok := run.RolePlanSnapshot["role_instructions"]; ok {
			if m, ok := raw.(map[string]any); ok {
				for role := range m {
					r := strings.ToLower(strings.TrimSpace(role))
					if r != "" {
						roles[r] = true
					}
				}
			}
		}
		if raw, ok := run.RolePlanSnapshot["role_graph"]; ok && raw != nil {
			var edges []any
			switch t := raw.(type) {
			case []any:
				edges = t
			case map[string]any:
				if arr, ok := t["edges"].([]any); ok {
					edges = arr
				}
			}
			for _, item := range edges {
				m, ok := item.(map[string]any)
				if !ok {
					continue
				}
				from := strings.TrimSpace(asString(m["from_role"]))
				if from == "" {
					from = strings.TrimSpace(asString(m["from"]))
				}
				to := strings.TrimSpace(asString(m["to_role"]))
				if to == "" {
					to = strings.TrimSpace(asString(m["to"]))
				}
				if from != "" {
					roles[strings.ToLower(from)] = true
				}
				if to != "" {
					roles[strings.ToLower(to)] = true
				}
			}
		}
	}
	if len(roles) == 0 {
		return nil
	}
	out := make([]string, 0, len(roles))
	for role := range roles {
		out = append(out, role)
	}
	sort.Strings(out)
	return out
}

func appendTeamRunHistory(meta map[string]any, teamRunID, status string) map[string]any {
	if meta == nil {
		meta = map[string]any{}
	}
	history := []map[string]any{}
	if raw, ok := meta["team_run_history"]; ok {
		if arr, ok := raw.([]any); ok {
			for _, item := range arr {
				if m, ok := item.(map[string]any); ok {
					history = append(history, m)
				}
			}
		} else if arr, ok := raw.([]map[string]any); ok {
			history = append(history, arr...)
		}
	}
	entry := map[string]any{
		"team_run_id":     teamRunID,
		"status":          status,
		"updated_unix_ms": time.Now().UTC().UnixMilli(),
	}
	history = append(history, entry)
	if len(history) > 50 {
		history = history[len(history)-50:]
	}
	meta["team_run_history"] = history
	return meta
}

func maybeEmitProgress(ctx context.Context, client *http.Client, cfg config, teamID, teamRunID string, status *teamRunResponse, meta map[string]any) (bool, error) {
	interval, ok := asInt(meta["progress_every_ms"])
	if !ok || interval <= 0 {
		return false, nil
	}
	now := time.Now().UTC().UnixMilli()
	lastRunID := strings.TrimSpace(asString(meta["last_progress_team_run_id"]))
	lastTS, _ := asInt(meta["last_progress_unix_ms"])
	if lastRunID != teamRunID {
		lastTS = 0
	}
	if lastTS > 0 && now-int64(lastTS) < int64(interval) {
		return false, nil
	}
	elapsed := int64(0)
	if status != nil && status.CreatedUnixMS > 0 {
		elapsed = now - status.CreatedUnixMS
	}
	payload := map[string]any{
		"type":       "progress",
		"message":    "orchestrator heartbeat",
		"ts_unix_ms": now,
		"data": map[string]any{
			"elapsed_ms": elapsed,
		},
	}
	if err := emitTeamRunGoalEvent(ctx, client, cfg, teamID, teamRunID, payload); err != nil {
		return false, err
	}
	meta["last_progress_unix_ms"] = now
	meta["last_progress_team_run_id"] = teamRunID
	return true, nil
}
