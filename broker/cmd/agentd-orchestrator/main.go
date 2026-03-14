package main

import (
	"context"
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
	return updateOrchestratorRunFields(ctx, client, cfg, teamID, runID, status, meta, expectedOwner, expectedStatus, nil, nil, nil)
}

func updateOrchestratorRunFields(
	ctx context.Context,
	client *http.Client,
	cfg config,
	teamID,
	runID,
	status string,
	meta map[string]any,
	expectedOwner *string,
	expectedStatus *string,
	goal *string,
	goalContract map[string]any,
	rolePlanSnapshot map[string]any,
) (*orchestratorRunResponse, error) {
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
	if goal != nil && strings.TrimSpace(*goal) != "" {
		payload["goal"] = strings.TrimSpace(*goal)
	}
	if goalContract != nil {
		payload["goal_contract"] = goalContract
	}
	if rolePlanSnapshot != nil {
		payload["role_plan_snapshot"] = rolePlanSnapshot
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
