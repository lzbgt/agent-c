package main

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	"os"
	"sort"
	"strings"
	"time"
)

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

func cancelTeamRun(ctx context.Context, client *http.Client, cfg config, teamID, teamRunID string) error {
	if teamID == "" || teamRunID == "" {
		return fmt.Errorf("missing team_id or team_run_id")
	}
	url := fmt.Sprintf("%s/v1/teams/%s/runs/%s/cancel", cfg.brokerBase, teamID, teamRunID)
	var resp map[string]any
	return doJSON(ctx, client, cfg, http.MethodPost, url, map[string]any{}, &resp)
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
	meta["runtime_members_retired_team_run_id"] = teamRunID
	meta["runtime_members_retired_unix_ms"] = time.Now().UTC().UnixMilli()
	meta["runtime_members_retired_status"] = retireStatus
	return true, nil
}
