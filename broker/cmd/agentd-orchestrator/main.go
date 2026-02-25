package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"net/http"
	"net/url"
	"os"
	"os/signal"
	"sort"
	"strings"
	"time"
)

func main() {
	cfg := parseFlags()
	if err := cfg.validate(); err != nil {
		fmt.Fprintf(os.Stderr, "config error: %v\n", err)
		os.Exit(2)
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt)
	defer stop()

	client := httpClient(cfg.insecureTLS)
	for {
		if err := runOnce(ctx, client, cfg); err != nil {
			fmt.Fprintf(os.Stderr, "orchestrator error: %v\n", err)
		}
		if cfg.once {
			return
		}
		select {
		case <-ctx.Done():
			return
		case <-time.After(cfg.pollInterval):
		}
	}
}

func parseFlags() config {
	var cfg config
	flag.StringVar(&cfg.brokerBase, "broker-base", strings.TrimSpace(os.Getenv("BROKER_BASE")), "Broker base URL (env: BROKER_BASE).")
	flag.StringVar(&cfg.oidcToken, "oidc-token", strings.TrimSpace(os.Getenv("BROKER_OIDC_TOKEN")), "Bearer token for broker auth (env: BROKER_OIDC_TOKEN).")
	flag.StringVar(&cfg.oidcTokenFile, "oidc-token-file", strings.TrimSpace(os.Getenv("BROKER_OIDC_TOKEN_FILE")), "Path to broker auth token file (env: BROKER_OIDC_TOKEN_FILE).")
	flag.BoolVar(&cfg.insecureTLS, "insecure", strings.TrimSpace(os.Getenv("BROKER_INSECURE_TLS")) == "1", "Skip TLS verification (env: BROKER_INSECURE_TLS=1).")
	flag.DurationVar(&cfg.pollInterval, "poll-interval", 5*time.Second, "Polling interval when not --once.")
	flag.BoolVar(&cfg.once, "once", false, "Process one poll cycle and exit.")
	flag.IntVar(&cfg.limit, "limit", 50, "Max orchestrator runs to fetch per team per poll.")
	flag.StringVar(&cfg.status, "status", "running", "Orchestrator run status filter (default: running).")
	flag.BoolVar(&cfg.includePlanned, "include-planned", true, "Also include planned runs (auto-start).")
	flag.StringVar(&cfg.orchestratorID, "orchestrator-id", strings.TrimSpace(os.Getenv("ORCHESTRATOR_ID")), "Orchestrator loop id stored in run meta.")
	flag.Parse()
	if cfg.orchestratorID == "" {
		cfg.orchestratorID = "orchestrator_" + randID(8)
	}
	return cfg
}

func (c config) validate() error {
	if c.brokerBase == "" {
		return fmt.Errorf("missing broker base url")
	}
	if c.oidcToken == "" && c.oidcTokenFile == "" {
		return fmt.Errorf("missing oidc token")
	}
	if c.limit <= 0 {
		c.limit = 50
	}
	if c.pollInterval <= 0 {
		c.pollInterval = 5 * time.Second
	}
	if c.status == "" {
		c.status = "running"
	}
	return nil
}

func (c config) bearerToken() (string, error) {
	if c.oidcTokenFile != "" {
		if data, err := os.ReadFile(c.oidcTokenFile); err == nil {
			token := strings.TrimSpace(string(data))
			if token != "" {
				return token, nil
			}
		} else if strings.TrimSpace(c.oidcToken) == "" {
			return "", fmt.Errorf("read oidc token file: %w", err)
		}
	}
	if strings.TrimSpace(c.oidcToken) == "" {
		return "", fmt.Errorf("missing oidc token")
	}
	return strings.TrimSpace(c.oidcToken), nil
}

func runOnce(ctx context.Context, client *http.Client, cfg config) error {
	teams, err := fetchTeams(ctx, client, cfg)
	if err != nil {
		return err
	}
	for _, team := range teams {
		teamID := strings.TrimSpace(team.TeamID)
		if teamID == "" {
			continue
		}
		runs := []orchestratorRun{}
		list, err := fetchOrchestratorRuns(ctx, client, cfg, teamID, cfg.status)
		if err != nil {
			return err
		}
		runs = append(runs, list...)
		if cfg.includePlanned && strings.ToLower(cfg.status) != "planned" {
			more, err := fetchOrchestratorRuns(ctx, client, cfg, teamID, "planned")
			if err != nil {
				return err
			}
			runs = append(runs, more...)
		}
		for _, run := range runs {
			if strings.TrimSpace(run.OrchestratorRunID) == "" {
				continue
			}
			if !shouldAutonomous(run.Meta) {
				continue
			}
			if err := handleRun(ctx, client, cfg, teamID, run); err != nil {
				fmt.Fprintf(os.Stderr, "orchestrator run %s error: %v\n", run.OrchestratorRunID, err)
			}
		}
	}
	return nil
}

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
		changed, err := tickActiveTeamRun(ctx, client, cfg, teamID, activeRunID, tr, meta)
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
	teamRunID string,
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
	if ok, err := maybeEmitDrift(ctx, client, cfg, teamID, teamRunID, status, meta); err != nil {
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

func maybeEmitDrift(ctx context.Context, client *http.Client, cfg config, teamID, teamRunID string, status *teamRunResponse, meta map[string]any) (bool, error) {
	threshold, ok := asInt(meta["drift_after_ms"])
	if !ok || threshold <= 0 {
		return false, nil
	}
	if status == nil || status.CreatedUnixMS <= 0 {
		return false, nil
	}
	now := time.Now().UTC().UnixMilli()
	elapsed := now - status.CreatedUnixMS
	if elapsed < int64(threshold) {
		return false, nil
	}
	lastRunID := strings.TrimSpace(asString(meta["last_drift_team_run_id"]))
	if lastRunID == teamRunID {
		return false, nil
	}
	payload := map[string]any{
		"type":       "drift",
		"message":    "orchestrator drift timeout",
		"ts_unix_ms": now,
		"data": map[string]any{
			"elapsed_ms":   elapsed,
			"threshold_ms": threshold,
		},
	}
	if err := emitTeamRunGoalEvent(ctx, client, cfg, teamID, teamRunID, payload); err != nil {
		return false, err
	}
	meta["last_drift_unix_ms"] = now
	meta["last_drift_team_run_id"] = teamRunID
	if applyDriftAction(ctx, client, cfg, teamID, teamRunID, meta, elapsed, int64(threshold), now) {
		meta["drift_action_unix_ms"] = now
		meta["drift_action_team_run_id"] = teamRunID
	}
	return true, nil
}

func applyDriftAction(
	ctx context.Context,
	client *http.Client,
	cfg config,
	teamID,
	teamRunID string,
	meta map[string]any,
	elapsed,
	threshold,
	now int64,
) bool {
	action := strings.ToLower(strings.TrimSpace(asString(meta["drift_action"])))
	if action == "" || action == "none" {
		return false
	}
	meta["drift_action"] = action
	switch action {
	case "guidance":
		kind := strings.TrimSpace(asString(meta["drift_guidance_kind"]))
		if kind == "" {
			kind = "warning"
		}
		priority := strings.TrimSpace(asString(meta["drift_guidance_priority"]))
		if priority == "" {
			priority = "high"
		}
		message := strings.TrimSpace(asString(meta["drift_guidance_message"]))
		if message == "" {
			message = fmt.Sprintf("Goal drift detected after %dms (threshold %dms).", elapsed, threshold)
		}
		payload := map[string]any{
			"source":          "drift_guard",
			"elapsed_ms":      elapsed,
			"threshold_ms":    threshold,
			"team_run_id":     teamRunID,
			"orchestrator_id": cfg.orchestratorID,
			"ts_unix_ms":      now,
		}
		if extra, ok := meta["drift_guidance_payload"].(map[string]any); ok && len(extra) > 0 {
			for k, v := range extra {
				payload[k] = v
			}
		}
		req := guidanceCreateRequest{
			TeamRunID: teamRunID,
			Kind:      kind,
			Priority:  priority,
			Message:   message,
			Payload:   payload,
		}
		targetRoles := cleanStringList(asStringSlice(meta["drift_guidance_target_roles"]))
		targetMembers := cleanStringList(asStringSlice(meta["drift_guidance_target_member_ids"]))
		targetAgents := cleanStringList(asStringSlice(meta["drift_guidance_target_agent_ids"]))
		targetOrch := strings.TrimSpace(asString(meta["drift_guidance_target_orchestrator"]))
		if len(targetRoles) > 0 {
			req.TargetRoles = targetRoles
		}
		if len(targetMembers) > 0 {
			req.TargetMemberIDs = targetMembers
		}
		if len(targetAgents) > 0 {
			req.TargetAgentIDs = targetAgents
		}
		if targetOrch == "" && len(targetRoles) == 0 && len(targetMembers) == 0 && len(targetAgents) == 0 {
			targetOrch = "human"
		}
		if targetOrch != "" {
			req.TargetOrchestrator = targetOrch
		}
		guidance, err := createGuidance(ctx, client, cfg, teamID, req)
		if err != nil {
			meta["drift_action_error"] = err.Error()
			fmt.Fprintf(os.Stderr, "drift guidance create failed: %v\n", err)
			return true
		}
		meta["drift_guidance_id"] = guidance.GuidanceID
		meta["drift_action_error"] = ""
		return true
	default:
		meta["drift_action_error"] = fmt.Sprintf("unknown drift_action: %s", action)
	}
	return true
}

func maybeProcessHandoffQueue(ctx context.Context, client *http.Client, cfg config, teamID, teamRunID string, meta map[string]any) (bool, error) {
	raw, ok := meta["handoff_queue"]
	if !ok || raw == nil {
		return false, nil
	}
	queue := []map[string]any{}
	switch t := raw.(type) {
	case []map[string]any:
		queue = append(queue, t...)
	case []any:
		for _, item := range t {
			if m, ok := item.(map[string]any); ok {
				queue = append(queue, m)
			}
		}
	}
	if len(queue) == 0 {
		return false, nil
	}
	ev := queue[0]
	payload := map[string]any{
		"from_role": strings.TrimSpace(asString(ev["from_role"])),
		"to_role":   strings.TrimSpace(asString(ev["to_role"])),
	}
	if payload["from_role"] == "" {
		payload["from_role"] = strings.TrimSpace(asString(ev["from"]))
	}
	if payload["to_role"] == "" {
		payload["to_role"] = strings.TrimSpace(asString(ev["to"]))
	}
	if payload["from_role"] == "" || payload["to_role"] == "" {
		queue = queue[1:]
		meta["handoff_queue"] = queue
		return true, nil
	}
	if v := strings.TrimSpace(asString(ev["reason"])); v != "" {
		payload["reason"] = v
	}
	if v := strings.TrimSpace(asString(ev["message"])); v != "" {
		payload["message"] = v
	}
	if v, ok := ev["data"].(map[string]any); ok && len(v) > 0 {
		payload["data"] = v
	}
	payload["ts_unix_ms"] = time.Now().UTC().UnixMilli()
	if err := dispatchHandoffDirective(ctx, client, cfg, teamID, teamRunID, payload); err != nil {
		if isGuidanceDispatchRetriable(err) {
			return false, nil
		}
		fmt.Fprintf(os.Stderr, "handoff directive dispatch failed: %v\n", err)
	}
	if err := emitTeamRunHandoffEvent(ctx, client, cfg, teamID, teamRunID, payload); err != nil {
		return false, err
	}
	queue = queue[1:]
	meta["handoff_queue"] = queue
	return true, nil
}

func shouldAutoAllocateRuntimeMembers(meta map[string]any, status *teamRunResponse) bool {
	if meta != nil {
		if v, ok := meta["auto_allocate_roles"]; ok {
			if b, ok := asBool(v); ok {
				return b
			}
		}
	}
	if status != nil {
		if b, ok := asBool(status.AutoAllocateRoles); ok {
			return b
		}
	}
	return true
}

func allocatorRetryAfter(meta map[string]any) int64 {
	if meta == nil {
		return 0
	}
	if v, ok := asInt64(meta["allocator_retry_after_ms"]); ok && v > 0 {
		return v
	}
	return 0
}

func shouldAttemptAllocator(meta map[string]any, teamRunID, missingSig string) bool {
	if meta == nil || teamRunID == "" || missingSig == "" {
		return true
	}
	lastRun := strings.TrimSpace(asString(meta["allocator_last_team_run_id"]))
	if lastRun != teamRunID {
		return true
	}
	lastSig := strings.TrimSpace(asString(meta["allocator_last_missing_signature"]))
	if lastSig != missingSig {
		return true
	}
	retryAfter := allocatorRetryAfter(meta)
	if retryAfter <= 0 {
		return false
	}
	lastTS, _ := asInt64(meta["allocator_last_unix_ms"])
	if lastTS <= 0 {
		return true
	}
	now := time.Now().UTC().UnixMilli()
	return now-int64(lastTS) >= retryAfter
}

func resolveAllocatorMissing(meta map[string]any, teamRunID string, missingRoles []string) []string {
	if meta == nil || teamRunID == "" {
		return normalizeRoles(missingRoles)
	}
	lastRun := strings.TrimSpace(asString(meta["allocator_last_team_run_id"]))
	if lastRun != teamRunID {
		return normalizeRoles(missingRoles)
	}
	lastSig := strings.TrimSpace(asString(meta["allocator_last_missing_signature"]))
	if lastSig != "" && lastSig != rolesSignature(missingRoles) {
		return normalizeRoles(missingRoles)
	}
	if _, ok := meta["allocator_missing_roles"]; ok {
		return normalizeRoles(asStringSlice(meta["allocator_missing_roles"]))
	}
	return normalizeRoles(missingRoles)
}

func maybeAllocateRuntimeMembers(
	ctx context.Context,
	client *http.Client,
	cfg config,
	teamID,
	teamRunID string,
	status *teamRunResponse,
	meta map[string]any,
	missingRoles []string,
) (bool, []string, error) {
	if len(missingRoles) == 0 || teamRunID == "" || teamID == "" {
		return false, missingRoles, nil
	}
	missingSig := rolesSignature(missingRoles)
	if !shouldAttemptAllocator(meta, teamRunID, missingSig) {
		return false, resolveAllocatorMissing(meta, teamRunID, missingRoles), nil
	}
	if meta == nil {
		meta = map[string]any{}
	}
	existing := parseRuntimeMembers(status.RuntimeMembers)
	maxMembers := 0
	if v, ok := asInt(meta["auto_allocate_max_members"]); ok && v > 0 {
		maxMembers = v
	} else if status != nil {
		if v, ok := asInt(status.AutoAllocateMaxMembers); ok && v > 0 {
			maxMembers = v
		}
	}
	resp, err := allocateRuntimeMembers(ctx, client, cfg, teamID, missingRoles, existing, maxMembers)
	now := time.Now().UTC().UnixMilli()
	meta["allocator_last_team_run_id"] = teamRunID
	meta["allocator_last_missing_signature"] = missingSig
	meta["allocator_last_unix_ms"] = now
	meta["allocator_last_missing_roles"] = missingRoles
	if err != nil {
		warn := allocatorWarningFromErr(err)
		if warn != "" {
			meta["allocator_warnings"] = []string{warn}
		}
		meta["allocator_allocated_roles"] = nil
		meta["allocator_missing_roles"] = missingRoles
		meta["allocator_runtime_members_added"] = 0
		if isAllocatorNonFatal(err) {
			return true, missingRoles, nil
		}
		return true, missingRoles, err
	}
	allocatedRoles := normalizeRoles(resp.AllocatedRoles)
	missingAfter := normalizeRoles(resp.MissingRoles)
	if missingAfter == nil {
		missingAfter = []string{}
	}
	warnings := cleanStringList(resp.Warnings)
	added := 0
	if len(resp.RuntimeMembers) > 0 {
		members := normalizeRuntimeMembers(resp.RuntimeMembers)
		if len(members) > 0 {
			if err := updateTeamRunRuntimeMembers(ctx, client, cfg, teamID, teamRunID, members); err != nil {
				return true, missingRoles, err
			}
			added = len(members)
		}
	}
	if len(allocatedRoles) > 0 {
		meta["allocator_allocated_roles"] = allocatedRoles
	} else {
		delete(meta, "allocator_allocated_roles")
	}
	meta["allocator_missing_roles"] = missingAfter
	if len(warnings) > 0 {
		meta["allocator_warnings"] = warnings
	} else {
		delete(meta, "allocator_warnings")
	}
	meta["allocator_runtime_members_added"] = added
	return true, missingAfter, nil
}

func allocatorWarningFromErr(err error) string {
	if err == nil {
		return ""
	}
	var httpErr *httpError
	if errors.As(err, &httpErr) {
		msg := strings.TrimSpace(httpErr.Body)
		if msg != "" {
			return msg
		}
		return fmt.Sprintf("allocator http %d", httpErr.Status)
	}
	return err.Error()
}

func isAllocatorNonFatal(err error) bool {
	if err == nil {
		return false
	}
	var httpErr *httpError
	if errors.As(err, &httpErr) {
		if httpErr.Status == http.StatusBadRequest {
			msg := strings.ToLower(httpErr.Body)
			if strings.Contains(msg, "no connected agents") {
				return true
			}
		}
	}
	return false
}

func shouldSpawnMissingRoles(meta map[string]any) bool {
	if meta == nil {
		return true
	}
	if v, ok := meta["spawn_missing_roles"]; ok {
		if b, ok := asBool(v); ok {
			return b
		}
	}
	return true
}

func shouldRetireRuntimeMembers(meta map[string]any) bool {
	if meta == nil {
		return false
	}
	if v, ok := meta["retire_runtime_members"]; ok {
		if b, ok := asBool(v); ok {
			return b
		}
	}
	return false
}

func runtimeMemberRetireStatus(meta map[string]any) string {
	if meta == nil {
		return "paused"
	}
	status := strings.ToLower(strings.TrimSpace(asString(meta["retire_runtime_member_status"])))
	if status == "" {
		status = "paused"
	}
	return status
}

type spawnMetaConfig struct {
	defaultCount       int
	countByRole        map[string]int
	requirements       map[string]any
	requirementsByRole map[string]map[string]any
	errors             []string
}

func parseSpawnMetaConfig(meta map[string]any) spawnMetaConfig {
	out := spawnMetaConfig{
		defaultCount:       1,
		countByRole:        map[string]int{},
		requirements:       map[string]any{},
		requirementsByRole: map[string]map[string]any{},
	}
	if meta == nil {
		return out
	}
	if v, ok := meta["spawn_count_per_role"]; ok {
		if n, ok := asInt(v); ok && n > 0 {
			out.defaultCount = n
		} else {
			out.errors = append(out.errors, "spawn_count_per_role must be a positive integer")
		}
	}
	if raw, ok := meta["spawn_count_by_role"]; ok {
		m, ok := raw.(map[string]any)
		if !ok {
			out.errors = append(out.errors, "spawn_count_by_role must be an object map")
		} else {
			for role, val := range m {
				r := strings.ToLower(strings.TrimSpace(role))
				if r == "" {
					continue
				}
				if n, ok := asInt(val); ok && n > 0 {
					out.countByRole[r] = n
				} else {
					out.errors = append(out.errors, fmt.Sprintf("spawn_count_by_role.%s must be a positive integer", r))
				}
			}
		}
	}
	if raw, ok := meta["spawn_requirements"]; ok {
		m, ok := raw.(map[string]any)
		if !ok {
			out.errors = append(out.errors, "spawn_requirements must be an object map")
		} else {
			for k, v := range m {
				out.requirements[k] = v
			}
		}
	}
	if raw, ok := meta["spawn_requirements_by_role"]; ok {
		m, ok := raw.(map[string]any)
		if !ok {
			out.errors = append(out.errors, "spawn_requirements_by_role must be an object map")
		} else {
			for role, val := range m {
				r := strings.ToLower(strings.TrimSpace(role))
				if r == "" {
					continue
				}
				roleMap, ok := val.(map[string]any)
				if !ok {
					out.errors = append(out.errors, fmt.Sprintf("spawn_requirements_by_role.%s must be an object map", r))
					continue
				}
				next := map[string]any{}
				for k, v := range roleMap {
					next[k] = v
				}
				if len(next) > 0 {
					out.requirementsByRole[r] = next
				}
			}
		}
	}
	return out
}

func shouldEmitSpawnValidation(meta map[string]any, teamRunID string, errors []string) bool {
	if len(errors) == 0 {
		return false
	}
	if meta == nil {
		return true
	}
	prevRun := strings.TrimSpace(asString(meta["spawn_validation_team_run_id"]))
	prevSig := strings.TrimSpace(asString(meta["spawn_validation_signature"]))
	if prevRun == teamRunID && prevSig == spawnValidationSignature(errors) {
		return false
	}
	return true
}

func spawnValidationSignature(errors []string) string {
	if len(errors) == 0 {
		return ""
	}
	cp := make([]string, len(errors))
	copy(cp, errors)
	sort.Strings(cp)
	return strings.Join(cp, " | ")
}

func recordSpawnValidation(meta map[string]any, teamRunID string, errors []string) map[string]any {
	if meta == nil {
		meta = map[string]any{}
	}
	meta["spawn_validation_errors"] = errors
	meta["spawn_validation_team_run_id"] = teamRunID
	meta["spawn_validation_signature"] = spawnValidationSignature(errors)
	meta["spawn_validation_unix_ms"] = time.Now().UTC().UnixMilli()
	return meta
}

func ensureSpawnRequests(ctx context.Context, client *http.Client, cfg config, teamID, orchestratorRunID, teamRunID string, meta map[string]any, missingRoles []string, owner string) error {
	if len(missingRoles) == 0 {
		return nil
	}
	existing, err := listSpawnRequests(ctx, client, cfg, teamID, orchestratorRunID)
	if err != nil {
		return err
	}
	existingByRole := map[string]int{}
	for _, req := range existing {
		role := strings.ToLower(strings.TrimSpace(req.Role))
		if role == "" {
			continue
		}
		status := strings.ToLower(strings.TrimSpace(req.Status))
		if status == "" || status == "error" {
			continue
		}
		count := req.Count
		if count <= 0 {
			count = 1
		}
		existingByRole[role] += count
	}
	created := map[string][]string{}
	if raw, ok := meta["spawn_requests"]; ok {
		if m, ok := raw.(map[string]any); ok {
			for role, val := range m {
				ids := asStringSlice(val)
				if len(ids) > 0 {
					created[role] = append(created[role], ids...)
				}
			}
		}
	}
	config := parseSpawnMetaConfig(meta)
	if len(config.errors) > 0 {
		if shouldEmitSpawnValidation(meta, teamRunID, config.errors) {
			meta = recordSpawnValidation(meta, teamRunID, config.errors)
			if teamRunID != "" {
				payload := map[string]any{
					"type":       "spawn_validation",
					"message":    "invalid spawn meta",
					"ts_unix_ms": time.Now().UTC().UnixMilli(),
					"data": map[string]any{
						"errors":              config.errors,
						"orchestrator_run_id": orchestratorRunID,
					},
				}
				_ = emitTeamRunGoalEvent(ctx, client, cfg, teamID, teamRunID, payload)
			}
			_, _ = updateOrchestratorRun(ctx, client, cfg, teamID, orchestratorRunID, "", meta, stringPtr(owner), nil)
		}
	}
	defaultCount := config.defaultCount
	countByRole := config.countByRole
	requirements := config.requirements
	requirementsByRole := config.requirementsByRole
	baseMeta := map[string]any{"orchestrator_id": cfg.orchestratorID, "reason": "missing_role"}
	if raw, ok := meta["spawn_request_meta"].(map[string]any); ok {
		for k, v := range raw {
			baseMeta[k] = v
		}
	}
	for _, role := range missingRoles {
		r := strings.ToLower(strings.TrimSpace(role))
		if r == "" {
			continue
		}
		desiredCount := defaultCount
		if v, ok := countByRole[r]; ok && v > 0 {
			desiredCount = v
		}
		if desiredCount <= 0 {
			continue
		}
		existingCount := existingByRole[r]
		if existingCount >= desiredCount {
			continue
		}
		requestCount := desiredCount - existingCount
		payload := map[string]any{
			"role":                r,
			"count":               requestCount,
			"status":              "requested",
			"orchestrator_run_id": orchestratorRunID,
			"meta":                baseMeta,
		}
		mergedRequirements := map[string]any{}
		for k, v := range requirements {
			mergedRequirements[k] = v
		}
		if roleReq, ok := requirementsByRole[r]; ok {
			for k, v := range roleReq {
				mergedRequirements[k] = v
			}
		}
		if len(mergedRequirements) > 0 {
			payload["requirements"] = mergedRequirements
		}
		resp, err := createSpawnRequest(ctx, client, cfg, teamID, payload)
		if err != nil {
			fmt.Fprintf(os.Stderr, "spawn request for role %s failed: %v\n", r, err)
			continue
		}
		if resp.SpawnRequestID != "" {
			created[r] = append(created[r], resp.SpawnRequestID)
		}
	}
	if len(created) > 0 {
		meta["spawn_requests"] = created
		_, _ = updateOrchestratorRun(ctx, client, cfg, teamID, orchestratorRunID, "", meta, stringPtr(owner), nil)
	}
	return nil
}

func parseRuntimeMembers(raw any) []map[string]any {
	out := []map[string]any{}
	switch t := raw.(type) {
	case []map[string]any:
		out = append(out, t...)
	case []any:
		for _, item := range t {
			if m, ok := item.(map[string]any); ok {
				out = append(out, m)
			}
		}
	}
	return out
}

func normalizeRuntimeMembers(raw []map[string]any) []map[string]any {
	if len(raw) == 0 {
		return nil
	}
	out := make([]map[string]any, 0, len(raw))
	for _, rm := range raw {
		if rm == nil {
			continue
		}
		agentID := strings.TrimSpace(asString(rm["agent_id"]))
		role := strings.TrimSpace(asString(rm["role"]))
		if agentID == "" || role == "" {
			continue
		}
		entry := map[string]any{
			"agent_id": agentID,
			"role":     role,
		}
		if v := strings.TrimSpace(asString(rm["deployment_id"])); v != "" {
			entry["deployment_id"] = v
		}
		if v := strings.TrimSpace(asString(rm["status"])); v != "" {
			entry["status"] = v
		}
		out = append(out, entry)
	}
	return out
}

func buildRuntimeMemberUpdates(runtimeMembers []map[string]any, status string) []map[string]any {
	if len(runtimeMembers) == 0 {
		return nil
	}
	updates := make([]map[string]any, 0, len(runtimeMembers))
	for _, rm := range runtimeMembers {
		memberID := strings.TrimSpace(asString(rm["member_id"]))
		agentID := strings.TrimSpace(asString(rm["agent_id"]))
		role := strings.TrimSpace(asString(rm["role"]))
		if memberID == "" || agentID == "" || role == "" {
			continue
		}
		entry := map[string]any{
			"member_id": memberID,
			"agent_id":  agentID,
			"role":      role,
			"status":    status,
		}
		if v := strings.TrimSpace(asString(rm["deployment_id"])); v != "" {
			entry["deployment_id"] = v
		}
		updates = append(updates, entry)
	}
	return updates
}

func updateTeamRunRuntimeMembers(ctx context.Context, client *http.Client, cfg config, teamID, teamRunID string, members []map[string]any) error {
	if teamID == "" || teamRunID == "" {
		return fmt.Errorf("missing team_id or team_run_id")
	}
	payload := map[string]any{
		"mode":            "merge",
		"runtime_members": members,
	}
	url := fmt.Sprintf("%s/v1/teams/%s/runs/%s/runtime_members", cfg.brokerBase, teamID, teamRunID)
	var resp map[string]any
	return doJSON(ctx, client, cfg, http.MethodPatch, url, payload, &resp)
}

func maybeRetireRuntimeMembers(ctx context.Context, client *http.Client, cfg config, teamID, teamRunID string, status *teamRunResponse, meta map[string]any) (bool, error) {
	if !shouldRetireRuntimeMembers(meta) {
		return false, nil
	}
	if status == nil {
		return false, nil
	}
	if strings.TrimSpace(teamRunID) == "" {
		return false, nil
	}
	lastRunID := strings.TrimSpace(asString(meta["runtime_members_retired_team_run_id"]))
	if lastRunID == teamRunID {
		return false, nil
	}
	runtimeMembers := parseRuntimeMembers(status.RuntimeMembers)
	if len(runtimeMembers) == 0 {
		return false, nil
	}
	retireStatus := runtimeMemberRetireStatus(meta)
	updates := buildRuntimeMemberUpdates(runtimeMembers, retireStatus)
	if len(updates) == 0 {
		return false, nil
	}
	if err := updateTeamRunRuntimeMembers(ctx, client, cfg, teamID, teamRunID, updates); err != nil {
		return false, err
	}
	if meta == nil {
		meta = map[string]any{}
	}
	meta["runtime_members_retired_team_run_id"] = teamRunID
	meta["runtime_members_retired_status"] = retireStatus
	meta["runtime_members_retired_unix_ms"] = time.Now().UTC().UnixMilli()
	return true, nil
}

func emitTeamRunGoalEvent(ctx context.Context, client *http.Client, cfg config, teamID, teamRunID string, event map[string]any) error {
	payload := map[string]any{
		"event": event,
	}
	url := fmt.Sprintf("%s/v1/teams/%s/runs/%s/goal", cfg.brokerBase, teamID, teamRunID)
	var resp map[string]any
	if err := doJSON(ctx, client, cfg, http.MethodPatch, url, payload, &resp); err != nil {
		return err
	}
	return nil
}

func emitTeamRunHandoffEvent(ctx context.Context, client *http.Client, cfg config, teamID, teamRunID string, event map[string]any) error {
	payload := map[string]any{
		"event": event,
	}
	url := fmt.Sprintf("%s/v1/teams/%s/runs/%s/handoff", cfg.brokerBase, teamID, teamRunID)
	var resp map[string]any
	if err := doJSON(ctx, client, cfg, http.MethodPatch, url, payload, &resp); err != nil {
		return err
	}
	return nil
}

func fetchTeams(ctx context.Context, client *http.Client, cfg config) ([]teamRow, error) {
	var resp teamListResponse
	if err := doJSON(ctx, client, cfg, http.MethodGet, cfg.brokerBase+"/v1/teams", nil, &resp); err != nil {
		return nil, err
	}
	if !resp.OK {
		return nil, fmt.Errorf("broker returned ok=false for teams")
	}
	return resp.Teams, nil
}

func fetchOrchestratorRuns(ctx context.Context, client *http.Client, cfg config, teamID, status string) ([]orchestratorRun, error) {
	qs := url.Values{}
	if status != "" {
		qs.Set("status", status)
	}
	if cfg.limit > 0 {
		qs.Set("limit", fmt.Sprintf("%d", cfg.limit))
	}
	url := fmt.Sprintf("%s/v1/teams/%s/orchestrator/runs", cfg.brokerBase, teamID)
	if encoded := qs.Encode(); encoded != "" {
		url += "?" + encoded
	}
	var resp orchestratorRunListResponse
	if err := doJSON(ctx, client, cfg, http.MethodGet, url, nil, &resp); err != nil {
		return nil, err
	}
	if !resp.OK {
		return nil, fmt.Errorf("broker returned ok=false for orchestrator runs")
	}
	return resp.Runs, nil
}

func heartbeatRun(ctx context.Context, client *http.Client, cfg config, teamID, runID, status string, expectedOwner *string, expectedStatus *string) (*orchestratorRunResponse, error) {
	payload := map[string]any{}
	if status != "" {
		payload["status"] = status
	}
	if expectedOwner != nil {
		payload["expected_owner"] = *expectedOwner
	}
	if expectedStatus != nil {
		payload["expected_status"] = *expectedStatus
	}
	url := fmt.Sprintf("%s/v1/teams/%s/orchestrator/runs/%s/heartbeat", cfg.brokerBase, teamID, runID)
	var resp orchestratorRunResponse
	if err := doJSON(ctx, client, cfg, http.MethodPost, url, payload, &resp); err != nil {
		return nil, err
	}
	return &resp, nil
}

func updateOrchestratorRun(ctx context.Context, client *http.Client, cfg config, teamID, runID, status string, meta map[string]any, expectedOwner *string, expectedStatus *string) (*orchestratorRunResponse, error) {
	payload := map[string]any{}
	if status != "" {
		payload["status"] = status
	}
	if meta != nil {
		payload["meta"] = meta
	}
	if expectedOwner != nil {
		payload["expected_owner"] = *expectedOwner
	}
	if expectedStatus != nil {
		payload["expected_status"] = *expectedStatus
	}
	url := fmt.Sprintf("%s/v1/teams/%s/orchestrator/runs/%s", cfg.brokerBase, teamID, runID)
	var resp orchestratorRunResponse
	if err := doJSON(ctx, client, cfg, http.MethodPatch, url, payload, &resp); err != nil {
		return nil, err
	}
	return &resp, nil
}

func createTeamRun(ctx context.Context, client *http.Client, cfg config, teamID string, runPayload, teamMeta map[string]any) (*teamRunResponse, error) {
	payload := map[string]any{
		"run": runPayload,
	}
	if teamMeta != nil {
		payload["team"] = teamMeta
	}
	url := fmt.Sprintf("%s/v1/teams/%s/runs", cfg.brokerBase, teamID)
	var resp teamRunResponse
	if err := doJSON(ctx, client, cfg, http.MethodPost, url, payload, &resp); err != nil {
		return nil, err
	}
	return &resp, nil
}

func fetchTeamRunStatus(ctx context.Context, client *http.Client, cfg config, teamID, teamRunID string) (*teamRunResponse, error) {
	url := fmt.Sprintf("%s/v1/teams/%s/runs/%s", cfg.brokerBase, teamID, teamRunID)
	var resp teamRunResponse
	if err := doJSON(ctx, client, cfg, http.MethodGet, url, nil, &resp); err != nil {
		return nil, err
	}
	return &resp, nil
}

func allocateRuntimeMembers(ctx context.Context, client *http.Client, cfg config, teamID string, roles []string, existing []map[string]any, maxMembers int) (*runtimeAllocateResponse, error) {
	payload := map[string]any{
		"roles":            roles,
		"prefer_connected": true,
	}
	if len(existing) > 0 {
		payload["existing_runtime_members"] = existing
	}
	if maxMembers > 0 {
		payload["max_members"] = maxMembers
	}
	payload["exclude_team_members"] = true
	url := fmt.Sprintf("%s/v1/teams/%s/runtime_members/allocate", cfg.brokerBase, teamID)
	var resp runtimeAllocateResponse
	if err := doJSON(ctx, client, cfg, http.MethodPost, url, payload, &resp); err != nil {
		return nil, err
	}
	if !resp.OK {
		return &resp, fmt.Errorf("broker returned ok=false for runtime member allocation")
	}
	return &resp, nil
}

func listSpawnRequests(ctx context.Context, client *http.Client, cfg config, teamID, orchestratorRunID string) ([]spawnRequest, error) {
	qs := url.Values{}
	if orchestratorRunID != "" {
		qs.Set("orchestrator_run_id", orchestratorRunID)
	}
	qs.Set("limit", "200")
	url := fmt.Sprintf("%s/v1/teams/%s/orchestrator/spawn_requests?%s", cfg.brokerBase, teamID, qs.Encode())
	var resp spawnListResponse
	if err := doJSON(ctx, client, cfg, http.MethodGet, url, nil, &resp); err != nil {
		return nil, err
	}
	if !resp.OK {
		return nil, fmt.Errorf("broker returned ok=false for spawn requests")
	}
	return resp.SpawnRequests, nil
}

func createSpawnRequest(ctx context.Context, client *http.Client, cfg config, teamID string, payload map[string]any) (*spawnRequest, error) {
	url := fmt.Sprintf("%s/v1/teams/%s/orchestrator/spawn_requests", cfg.brokerBase, teamID)
	var resp spawnCreateResponse
	if err := doJSON(ctx, client, cfg, http.MethodPost, url, payload, &resp); err != nil {
		return nil, err
	}
	if !resp.OK {
		return nil, fmt.Errorf("broker returned ok=false for spawn request")
	}
	return &resp.SpawnRequest, nil
}
