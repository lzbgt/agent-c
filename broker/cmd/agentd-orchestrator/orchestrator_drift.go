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

func maybeEmitDrift(
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
	if applyDriftAction(ctx, client, cfg, teamID, teamRunID, orchestratorRunID, owner, run, status, meta, elapsed, int64(threshold), now) {
		meta["drift_action_unix_ms"] = now
		meta["drift_action_team_run_id"] = teamRunID
	}
	return true, nil
}

func buildGuidanceBriefing(
	run orchestratorRun,
	status *teamRunResponse,
	teamRunID string,
	action string,
	elapsed,
	threshold,
	now int64,
	meta map[string]any,
) map[string]any {
	if meta == nil {
		meta = map[string]any{}
	}
	briefing := map[string]any{
		"version": 1,
		"drift": map[string]any{
			"elapsed_ms":       elapsed,
			"threshold_ms":     threshold,
			"detected_unix_ms": now,
		},
	}
	if teamRunID != "" {
		briefing["team_run_id"] = teamRunID
	}
	if action != "" {
		briefing["drift_action"] = action
	}
	if goal := strings.TrimSpace(run.Goal); goal != "" {
		briefing["goal"] = goal
	}
	if run.GoalContract != nil && len(run.GoalContract) > 0 {
		briefing["goal_contract"] = run.GoalContract
	} else if status != nil {
		if gc, ok := status.GoalContract.(map[string]any); ok && len(gc) > 0 {
			briefing["goal_contract"] = gc
		}
	}
	if run.RolePlanSnapshot != nil && len(run.RolePlanSnapshot) > 0 {
		briefing["role_plan_snapshot"] = run.RolePlanSnapshot
	}
	if status != nil {
		if v := strings.TrimSpace(status.Status); v != "" {
			briefing["team_run_status"] = v
		}
		if status.CreatedUnixMS > 0 {
			briefing["team_run_created_unix_ms"] = status.CreatedUnixMS
			if now > status.CreatedUnixMS {
				briefing["team_run_elapsed_ms"] = now - status.CreatedUnixMS
			}
		}
	}
	proposed := map[string]any{}
	if goal := strings.TrimSpace(asString(meta["replan_goal"])); goal != "" {
		proposed["goal"] = goal
	}
	if gc, ok := meta["replan_goal_contract"].(map[string]any); ok && len(gc) > 0 {
		proposed["goal_contract"] = gc
	}
	if rp, ok := meta["replan_role_plan_snapshot"].(map[string]any); ok && len(rp) > 0 {
		proposed["role_plan_snapshot"] = rp
	}
	if len(proposed) > 0 {
		briefing["proposed"] = proposed
	}
	return briefing
}

func applyDriftAction(
	ctx context.Context,
	client *http.Client,
	cfg config,
	teamID,
	teamRunID string,
	orchestratorRunID string,
	owner string,
	run orchestratorRun,
	status *teamRunResponse,
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
	buildGuidance := func(prefix, defaultKind, defaultPriority, defaultMessage string, includeReplan bool) guidanceCreateRequest {
		kind := strings.TrimSpace(asString(meta[prefix+"_kind"]))
		if kind == "" {
			kind = defaultKind
		}
		priority := strings.TrimSpace(asString(meta[prefix+"_priority"]))
		if priority == "" {
			priority = defaultPriority
		}
		message := strings.TrimSpace(asString(meta[prefix+"_message"]))
		if message == "" {
			message = defaultMessage
		}
		payload := map[string]any{
			"source":              "drift_guard",
			"elapsed_ms":          elapsed,
			"threshold_ms":        threshold,
			"team_run_id":         teamRunID,
			"orchestrator_id":     cfg.orchestratorID,
			"orchestrator_run_id": orchestratorRunID,
			"drift_action":        action,
			"ts_unix_ms":          now,
		}
		if briefing := buildGuidanceBriefing(run, status, teamRunID, action, elapsed, threshold, now, meta); len(briefing) > 0 {
			payload["briefing"] = briefing
		}
		if includeReplan {
			payload["replan_requested"] = true
		}
		if extra, ok := meta[prefix+"_payload"].(map[string]any); ok && len(extra) > 0 {
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
		targetRoles := cleanStringList(asStringSlice(meta[prefix+"_target_roles"]))
		targetMembers := cleanStringList(asStringSlice(meta[prefix+"_target_member_ids"]))
		targetAgents := cleanStringList(asStringSlice(meta[prefix+"_target_agent_ids"]))
		targetOrch := strings.TrimSpace(asString(meta[prefix+"_target_orchestrator"]))
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
		return req
	}
	switch action {
	case "guidance":
		req := buildGuidance(
			"drift_guidance",
			"warning",
			"high",
			fmt.Sprintf("Goal drift detected after %dms (threshold %dms).", elapsed, threshold),
			false,
		)
		guidance, err := createGuidance(ctx, client, cfg, teamID, req)
		if err != nil {
			meta["drift_action_error"] = err.Error()
			fmt.Fprintf(os.Stderr, "drift guidance create failed: %v\n", err)
			return true
		}
		meta["drift_guidance_id"] = guidance.GuidanceID
		meta["drift_action_error"] = ""
		return true
	case "cancel":
		if teamRunID == "" {
			meta["drift_action_error"] = "drift cancel missing team_run_id"
			return true
		}
		if err := cancelTeamRun(ctx, client, cfg, teamID, teamRunID); err != nil {
			meta["drift_action_error"] = err.Error()
			fmt.Fprintf(os.Stderr, "drift cancel failed: %v\n", err)
			return true
		}
		meta["drift_cancel_unix_ms"] = now
		meta["drift_cancel_team_run_id"] = teamRunID
		meta["drift_action_error"] = ""
		return true
	case "pause":
		if orchestratorRunID == "" {
			meta["drift_action_error"] = "drift pause missing orchestrator_run_id"
			return true
		}
		if _, err := updateOrchestratorRun(ctx, client, cfg, teamID, orchestratorRunID, "paused", meta, stringPtr(owner), nil); err != nil {
			meta["drift_action_error"] = err.Error()
			fmt.Fprintf(os.Stderr, "drift pause failed: %v\n", err)
			return true
		}
		meta["drift_pause_unix_ms"] = now
		meta["drift_pause_team_run_id"] = teamRunID
		meta["drift_action_error"] = ""
		return true
	case "replan":
		pauseErr := ""
		if orchestratorRunID == "" {
			pauseErr = "drift replan missing orchestrator_run_id"
		} else if _, err := updateOrchestratorRun(ctx, client, cfg, teamID, orchestratorRunID, "paused", meta, stringPtr(owner), nil); err != nil {
			pauseErr = err.Error()
			fmt.Fprintf(os.Stderr, "drift replan pause failed: %v\n", err)
		} else {
			meta["drift_replan_pause_unix_ms"] = now
			meta["drift_replan_pause_team_run_id"] = teamRunID
		}
		req := buildGuidance(
			"drift_replan",
			"directive",
			"urgent",
			fmt.Sprintf("Goal drift detected after %dms (threshold %dms). Please replan.", elapsed, threshold),
			true,
		)
		guidance, err := createGuidance(ctx, client, cfg, teamID, req)
		if err != nil {
			meta["drift_action_error"] = err.Error()
			if pauseErr != "" {
				meta["drift_action_error"] = pauseErr + " | " + err.Error()
			}
			fmt.Fprintf(os.Stderr, "drift replan guidance create failed: %v\n", err)
			return true
		}
		meta["drift_replan_guidance_id"] = guidance.GuidanceID
		meta["drift_action_error"] = pauseErr
		return true
	default:
		meta["drift_action_error"] = fmt.Sprintf("unknown drift_action: %s", action)
	}
	return true
}

func maybeHandleReplanAck(
	ctx context.Context,
	client *http.Client,
	cfg config,
	teamID,
	orchestratorRunID,
	owner string,
	run orchestratorRun,
	meta map[string]any,
) (bool, error) {
	if meta == nil {
		return false, nil
	}
	if strings.ToLower(strings.TrimSpace(asString(meta["drift_action"]))) != "replan" {
		return false, nil
	}
	if _, ok := asInt64(meta["drift_replan_ack_unix_ms"]); ok {
		return false, nil
	}
	guidanceID := strings.TrimSpace(asString(meta["drift_replan_guidance_id"]))
	if guidanceID == "" {
		return false, nil
	}
	guidance, err := getGuidance(ctx, client, cfg, teamID, guidanceID)
	if err != nil {
		return false, err
	}
	if strings.ToLower(strings.TrimSpace(guidance.Status)) != "acked" {
		return false, nil
	}
	receiptsRequired, minAck, roleSet, sourceSet, requireAllRoles := replanReceiptRequirements(meta)
	if receiptsRequired {
		receipts, err := listGuidanceReceipts(ctx, client, cfg, teamID, guidanceID, 200)
		if err != nil {
			return false, err
		}
		ok, count, rolesSeen, sourcesSeen := replanReceiptsSatisfied(receipts, minAck, roleSet, sourceSet, requireAllRoles)
		meta["replan_ack_count"] = count
		if len(rolesSeen) > 0 {
			meta["replan_ack_roles_seen"] = rolesSeen
		}
		if len(sourcesSeen) > 0 {
			meta["replan_ack_sources_seen"] = sourcesSeen
		}
		if !ok {
			return false, nil
		}
		meta["replan_ack_satisfied_unix_ms"] = time.Now().UTC().UnixMilli()
	}
	if guidance.AckedUnixMS > 0 {
		meta["drift_replan_ack_unix_ms"] = guidance.AckedUnixMS
	} else {
		meta["drift_replan_ack_unix_ms"] = time.Now().UTC().UnixMilli()
	}
	if guidance.AckedBy != "" {
		meta["drift_replan_ack_by"] = guidance.AckedBy
	}
	if guidance.AckNote != "" {
		meta["drift_replan_ack_note"] = guidance.AckNote
	}
	if _, ok := meta["replan_prev_goal"]; !ok {
		prev := strings.TrimSpace(run.Goal)
		if prev != "" {
			meta["replan_prev_goal"] = prev
		}
	}
	if _, ok := meta["replan_prev_goal_contract"]; !ok && len(run.GoalContract) > 0 {
		meta["replan_prev_goal_contract"] = run.GoalContract
	}
	if _, ok := meta["replan_prev_role_plan_snapshot"]; !ok && len(run.RolePlanSnapshot) > 0 {
		meta["replan_prev_role_plan_snapshot"] = run.RolePlanSnapshot
	}
	activeRunID := strings.TrimSpace(asString(meta["active_team_run_id"]))
	goal := strings.TrimSpace(asString(meta["replan_goal"]))
	var goalPtr *string
	appliedUnixMS := time.Now().UTC().UnixMilli()
	if goal != "" {
		goalPtr = &goal
		meta["replan_goal_applied_unix_ms"] = appliedUnixMS
	}
	var goalContract map[string]any
	if raw, ok := meta["replan_goal_contract"].(map[string]any); ok && len(raw) > 0 {
		goalContract = raw
		meta["replan_goal_contract_applied_unix_ms"] = appliedUnixMS
	}
	var rolePlan map[string]any
	if raw, ok := meta["replan_role_plan_snapshot"].(map[string]any); ok && len(raw) > 0 {
		rolePlan = raw
		meta["replan_role_plan_snapshot_applied_unix_ms"] = appliedUnixMS
	}
	createNew := false
	if v, ok := asBool(meta["replan_create_new_run"]); ok {
		createNew = v
	}
	cancelActive := false
	if v, ok := asBool(meta["replan_cancel_active_run"]); ok {
		cancelActive = v
	}
	if cancelActive && activeRunID != "" {
		if err := cancelTeamRun(ctx, client, cfg, teamID, activeRunID); err != nil {
			meta["replan_cancel_error"] = err.Error()
		} else {
			meta["replan_cancel_unix_ms"] = time.Now().UTC().UnixMilli()
		}
	}
	if createNew && activeRunID != "" {
		meta["replan_prev_team_run_id"] = activeRunID
		meta["active_team_run_id"] = ""
		meta["replan_new_run_requested_unix_ms"] = time.Now().UTC().UnixMilli()
	}
	eventRunID := activeRunID
	if eventRunID == "" {
		eventRunID = strings.TrimSpace(asString(meta["replan_prev_team_run_id"]))
	}
	if eventRunID != "" {
		eventData := map[string]any{
			"guidance_id": guidanceID,
		}
		if v, ok := meta["replan_ack_count"]; ok {
			eventData["ack_count"] = v
		}
		if v, ok := meta["replan_ack_roles_seen"]; ok {
			eventData["ack_roles"] = v
		}
		if v, ok := meta["replan_ack_sources_seen"]; ok {
			eventData["ack_sources"] = v
		}
		if guidance.AckedBy != "" {
			eventData["acked_by"] = guidance.AckedBy
		}
		if guidance.AckNote != "" {
			eventData["ack_note"] = guidance.AckNote
		}
		if guidance.AckedUnixMS > 0 {
			eventData["ack_unix_ms"] = guidance.AckedUnixMS
		}
		if prevGoal, ok := meta["replan_prev_goal"]; ok {
			eventData["prev_goal"] = prevGoal
		}
		if prevGoalContract, ok := meta["replan_prev_goal_contract"]; ok {
			eventData["prev_goal_contract"] = prevGoalContract
		}
		if prevRolePlan, ok := meta["replan_prev_role_plan_snapshot"]; ok {
			eventData["prev_role_plan_snapshot"] = prevRolePlan
		}
		if goalPtr != nil {
			eventData["goal"] = *goalPtr
		}
		if goalContract != nil {
			eventData["goal_contract"] = goalContract
		}
		if rolePlan != nil {
			eventData["role_plan_snapshot"] = rolePlan
			if !createNew {
				eventData["role_plan_requires_new_run"] = true
				meta["replan_role_plan_deferred_team_run_id"] = eventRunID
				meta["replan_role_plan_deferred_unix_ms"] = appliedUnixMS
			}
		}
		event := map[string]any{
			"type":       "replan_resume",
			"message":    "replan guidance acked",
			"ts_unix_ms": appliedUnixMS,
			"data":       eventData,
		}
		if !createNew && goalContract != nil {
			if err := updateTeamRunGoalState(ctx, client, cfg, teamID, eventRunID, goalContract, event); err != nil {
				meta["replan_team_goal_error"] = err.Error()
				meta["replan_event_error"] = err.Error()
			} else {
				meta["replan_team_goal_team_run_id"] = eventRunID
				meta["replan_team_goal_applied_unix_ms"] = appliedUnixMS
				meta["replan_event_unix_ms"] = event["ts_unix_ms"]
				delete(meta, "replan_team_goal_error")
				delete(meta, "replan_event_error")
			}
		} else if err := emitTeamRunGoalEvent(ctx, client, cfg, teamID, eventRunID, event); err != nil {
			meta["replan_event_error"] = err.Error()
		} else {
			meta["replan_event_unix_ms"] = event["ts_unix_ms"]
		}
	}
	resp, err := updateOrchestratorRunFields(ctx, client, cfg, teamID, orchestratorRunID, "running", meta, stringPtr(owner), stringPtr("paused"), goalPtr, goalContract, rolePlan)
	if err != nil {
		if isHTTPStatus(err, http.StatusConflict) {
			return false, nil
		}
		return false, err
	}
	if resp != nil && resp.Run.OrchestratorRunID != "" {
		return true, nil
	}
	return false, nil
}

func replanReceiptRequirements(meta map[string]any) (bool, int, map[string]bool, map[string]bool, bool) {
	minAck := 1
	if v, ok := asInt(meta["replan_ack_min"]); ok && v > 0 {
		minAck = v
	}
	roles := normalizeRoles(asStringSlice(meta["replan_ack_roles"]))
	roleSet := map[string]bool{}
	for _, r := range roles {
		roleSet[r] = true
	}
	sources := cleanStringList(asStringSlice(meta["replan_ack_sources"]))
	sourceSet := map[string]bool{}
	for _, s := range sources {
		sourceSet[strings.ToLower(strings.TrimSpace(s))] = true
	}
	requireAllRoles := false
	if v, ok := asBool(meta["replan_ack_all_roles"]); ok {
		requireAllRoles = v
	}
	required := len(roleSet) > 0 || len(sourceSet) > 0 || requireAllRoles || minAck > 1
	return required, minAck, roleSet, sourceSet, requireAllRoles
}

func replanReceiptsSatisfied(receipts []guidanceReceipt, minAck int, roleSet, sourceSet map[string]bool, requireAllRoles bool) (bool, int, []string, []string) {
	if minAck <= 0 {
		minAck = 1
	}
	if len(receipts) == 0 {
		if minAck <= 0 {
			return true, 0, nil, nil
		}
		return false, 0, nil, nil
	}
	seen := map[string]bool{}
	roleHits := map[string]bool{}
	sourceHits := map[string]bool{}
	count := 0
	for _, r := range receipts {
		role := strings.ToLower(strings.TrimSpace(r.AckRole))
		source := strings.ToLower(strings.TrimSpace(r.AckSource))
		if len(roleSet) > 0 && !roleSet[role] {
			continue
		}
		if len(sourceSet) > 0 && !sourceSet[source] {
			continue
		}
		key := strings.TrimSpace(r.AckBy)
		if key == "" {
			key = fmt.Sprintf("receipt:%d", r.ID)
		}
		if seen[key] {
			continue
		}
		seen[key] = true
		count++
		if role != "" {
			roleHits[role] = true
		}
		if source != "" {
			sourceHits[source] = true
		}
	}
	rolesSeen := []string{}
	for role := range roleHits {
		rolesSeen = append(rolesSeen, role)
	}
	sort.Strings(rolesSeen)
	sourcesSeen := []string{}
	for source := range sourceHits {
		sourcesSeen = append(sourcesSeen, source)
	}
	sort.Strings(sourcesSeen)
	if requireAllRoles && len(roleSet) > 0 {
		for role := range roleSet {
			if !roleHits[role] {
				return false, count, rolesSeen, sourcesSeen
			}
		}
		return true, count, rolesSeen, sourcesSeen
	}
	return count >= minAck, count, rolesSeen, sourcesSeen
}
