package main

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	"net/url"
	"strings"
	"time"
)

func processGuidance(
	ctx context.Context,
	client *http.Client,
	cfg config,
	teamID,
	teamRunID string,
	meta map[string]any,
) (bool, error) {
	if meta == nil {
		return false, nil
	}
	ackedIDs := asStringSlice(meta["guidance_ack_ids"])
	ackedSet := map[string]bool{}
	for _, id := range ackedIDs {
		id = strings.TrimSpace(id)
		if id == "" {
			continue
		}
		ackedSet[id] = true
	}
	sinceTS := int64(0)
	if v, ok := asInt64(meta["guidance_since_ts"]); ok && v > 0 {
		sinceTS = v
	}
	currentRunID := strings.TrimSpace(teamRunID)
	prevRunID := strings.TrimSpace(asString(meta["guidance_since_team_run_id"]))
	if currentRunID != "" && prevRunID != currentRunID {
		sinceTS = 0
	}
	globalSinceTS := int64(0)
	if v, ok := asInt64(meta["guidance_since_ts_global"]); ok && v > 0 {
		globalSinceTS = v
	}
	runItems, err := fetchGuidance(ctx, client, cfg, teamID, teamRunID, sinceTS)
	if err != nil {
		return false, err
	}
	globalItems, err := fetchGuidance(ctx, client, cfg, teamID, "", globalSinceTS)
	if err != nil {
		return false, err
	}
	if len(runItems) == 0 && len(globalItems) == 0 {
		return false, nil
	}
	items := make([]guidanceEvent, 0, len(runItems)+len(globalItems))
	items = append(items, runItems...)
	items = append(items, globalItems...)
	changed := false
	pendingRun := false
	pendingGlobal := false
	runMaxTS := sinceTS
	globalMaxTS := globalSinceTS
	nowUnixMS := time.Now().UTC().UnixMilli()
	for _, item := range items {
		id := strings.TrimSpace(item.GuidanceID)
		if id == "" {
			continue
		}
		if ackedSet[id] {
			continue
		}
		itemRunID := strings.TrimSpace(item.TeamRunID)
		if itemRunID != "" && teamRunID != "" && itemRunID != teamRunID {
			continue
		}
		if itemRunID == "" {
			if item.CreatedUnixMS > globalMaxTS {
				globalMaxTS = item.CreatedUnixMS
			}
		} else if item.CreatedUnixMS > runMaxTS {
			runMaxTS = item.CreatedUnixMS
		}
		status := strings.ToLower(strings.TrimSpace(item.Status))
		if status != "" && status != "open" {
			ackedSet[id] = true
			ackedIDs = append(ackedIDs, id)
			changed = true
			continue
		}
		if item.ExpiresUnixMS > 0 && nowUnixMS >= item.ExpiresUnixMS {
			note := fmt.Sprintf("expired by orchestrator %s", cfg.orchestratorID)
			if _, err := ackGuidance(ctx, client, cfg, teamID, id, "expired", note, "orchestrator", "orchestrator"); err != nil {
				if isHTTPStatus(err, http.StatusConflict) {
					ackedSet[id] = true
					ackedIDs = append(ackedIDs, id)
					changed = true
					continue
				}
				return false, err
			}
			ackedSet[id] = true
			ackedIDs = append(ackedIDs, id)
			changed = true
			continue
		}
		if item.TargetOrchestrator != "" && item.TargetOrchestrator != cfg.orchestratorID {
			continue
		}
		itemPending := false
		dispatchTargets, shouldDispatch := guidanceTargetsForMembers(item)
		dispatched := false
		if shouldDispatch {
			runID := teamRunID
			if item.TeamRunID != "" {
				runID = item.TeamRunID
			}
			if runID == "" {
				itemPending = true
			} else {
				if _, err := dispatchGuidanceDirective(ctx, client, cfg, teamID, runID, item, dispatchTargets); err != nil {
					if isGuidanceDispatchRetriable(err) {
						itemPending = true
					} else {
						return false, err
					}
				} else {
					dispatched = true
				}
			}
		}
		if shouldDispatch && !dispatched {
			if itemPending {
				if itemRunID == "" {
					pendingGlobal = true
				} else {
					pendingRun = true
				}
			}
			continue
		}
		shouldAck := shouldHandleGuidance(item, cfg.orchestratorID) || dispatched
		if !shouldAck {
			continue
		}
		note := fmt.Sprintf("received by orchestrator %s", cfg.orchestratorID)
		if dispatched {
			note = fmt.Sprintf("dispatched by orchestrator %s", cfg.orchestratorID)
		}
		if _, err := ackGuidance(ctx, client, cfg, teamID, id, "acked", note, "orchestrator", "orchestrator"); err != nil {
			if isHTTPStatus(err, http.StatusConflict) {
				ackedSet[id] = true
				ackedIDs = append(ackedIDs, id)
				changed = true
				continue
			}
			return false, err
		}
		ackedSet[id] = true
		ackedIDs = append(ackedIDs, id)
		changed = true
	}
	if runMaxTS > sinceTS && !pendingRun {
		meta["guidance_since_ts"] = runMaxTS
		changed = true
	}
	if currentRunID != "" && currentRunID != prevRunID {
		meta["guidance_since_team_run_id"] = currentRunID
		changed = true
	}
	if globalMaxTS > globalSinceTS && !pendingGlobal {
		meta["guidance_since_ts_global"] = globalMaxTS
		changed = true
	}
	if changed {
		const maxAcked = 200
		if len(ackedIDs) > maxAcked {
			ackedIDs = ackedIDs[len(ackedIDs)-maxAcked:]
		}
		meta["guidance_ack_ids"] = ackedIDs
	}
	return changed, nil
}

func shouldHandleGuidance(item guidanceEvent, orchestratorID string) bool {
	if item.TargetOrchestrator != "" && item.TargetOrchestrator != orchestratorID {
		return false
	}
	if len(item.TargetMemberIDs) > 0 || len(item.TargetAgentIDs) > 0 {
		return false
	}
	if len(item.TargetRoles) == 0 {
		return true
	}
	for _, r := range item.TargetRoles {
		if strings.EqualFold(strings.TrimSpace(r), "orchestrator") {
			return true
		}
	}
	return false
}

func guidanceTargetsForMembers(item guidanceEvent) (map[string]any, bool) {
	hasExplicit := len(item.TargetRoles) > 0 || len(item.TargetMemberIDs) > 0 || len(item.TargetAgentIDs) > 0
	roles := []string{}
	for _, r := range item.TargetRoles {
		role := strings.TrimSpace(r)
		if role == "" {
			continue
		}
		if strings.EqualFold(role, "orchestrator") {
			continue
		}
		roles = append(roles, role)
	}
	memberIDs := cleanStringList(item.TargetMemberIDs)
	agentIDs := cleanStringList(item.TargetAgentIDs)
	if len(roles) == 0 && len(memberIDs) == 0 && len(agentIDs) == 0 {
		if !hasExplicit {
			return nil, true
		}
		return nil, false
	}
	out := map[string]any{}
	if len(roles) > 0 {
		out["roles"] = roles
	}
	if len(memberIDs) > 0 {
		out["member_ids"] = memberIDs
	}
	if len(agentIDs) > 0 {
		out["agent_ids"] = agentIDs
	}
	return out, true
}

func fetchGuidance(ctx context.Context, client *http.Client, cfg config, teamID, teamRunID string, sinceTS int64) ([]guidanceEvent, error) {
	qs := url.Values{}
	qs.Set("status", "open")
	qs.Set("limit", "200")
	if teamRunID != "" {
		qs.Set("team_run_id", teamRunID)
	}
	if sinceTS > 0 {
		qs.Set("since_ts", fmt.Sprintf("%d", sinceTS))
	}
	url := fmt.Sprintf("%s/v1/teams/%s/guidance?%s", cfg.brokerBase, teamID, qs.Encode())
	var resp guidanceListResponse
	if err := doJSON(ctx, client, cfg, http.MethodGet, url, nil, &resp); err != nil {
		return nil, err
	}
	if !resp.OK {
		return nil, fmt.Errorf("broker returned ok=false for guidance list")
	}
	return resp.Guidance, nil
}

func ackGuidance(ctx context.Context, client *http.Client, cfg config, teamID, guidanceID, status, note, ackSource, ackRole string) (*guidanceAckResponse, error) {
	if strings.TrimSpace(status) == "" {
		status = "acked"
	}
	payload := map[string]any{
		"status":     status,
		"note":       note,
		"ack_source": ackSource,
		"ack_role":   ackRole,
	}
	url := fmt.Sprintf("%s/v1/teams/%s/guidance/%s/ack", cfg.brokerBase, teamID, guidanceID)
	var resp guidanceAckResponse
	if err := doJSON(ctx, client, cfg, http.MethodPost, url, payload, &resp); err != nil {
		return nil, err
	}
	if !resp.OK {
		return nil, fmt.Errorf("broker returned ok=false for guidance ack")
	}
	return &resp, nil
}

func dispatchGuidanceDirective(
	ctx context.Context,
	client *http.Client,
	cfg config,
	teamID,
	teamRunID string,
	item guidanceEvent,
	targets map[string]any,
) (*moderatorDispatchResponse, error) {
	payload := map[string]any{
		"directive":         item.Message,
		"append_to_session": true,
		"scope":             fmt.Sprintf("guidance:%s", item.GuidanceID),
		"actor": map[string]any{
			"id":   cfg.orchestratorID,
			"kind": "orchestrator",
		},
	}
	meta := map[string]any{
		"guidance_id":       item.GuidanceID,
		"guidance_kind":     item.Kind,
		"guidance_priority": item.Priority,
	}
	if item.TeamRunID != "" {
		meta["guidance_team_run_id"] = item.TeamRunID
	}
	if len(item.Payload) > 0 {
		meta["guidance_payload"] = item.Payload
	}
	if len(meta) > 0 {
		payload["metadata"] = meta
	}
	if targets != nil && len(targets) > 0 {
		payload["targets"] = targets
	}
	url := fmt.Sprintf("%s/v1/teams/%s/runs/%s/moderator/directive", cfg.brokerBase, teamID, teamRunID)
	var resp moderatorDispatchResponse
	if err := doJSON(ctx, client, cfg, http.MethodPost, url, payload, &resp); err != nil {
		return nil, err
	}
	if !resp.OK {
		return nil, fmt.Errorf("broker returned ok=false for moderator directive")
	}
	return &resp, nil
}

func dispatchHandoffDirective(
	ctx context.Context,
	client *http.Client,
	cfg config,
	teamID,
	teamRunID string,
	payload map[string]any,
) error {
	if payload == nil {
		return nil
	}
	fromRole := strings.TrimSpace(asString(payload["from_role"]))
	toRole := strings.TrimSpace(asString(payload["to_role"]))
	if fromRole == "" || toRole == "" {
		return nil
	}
	reason := strings.TrimSpace(asString(payload["reason"]))
	message := strings.TrimSpace(asString(payload["message"]))
	directive := fmt.Sprintf("Handoff %s -> %s", fromRole, toRole)
	if reason != "" {
		directive = fmt.Sprintf("%s (reason: %s)", directive, reason)
	}
	if message != "" {
		directive = fmt.Sprintf("%s\n%s", directive, message)
	}
	meta := map[string]any{
		"handoff_from_role": fromRole,
		"handoff_to_role":   toRole,
	}
	if reason != "" {
		meta["handoff_reason"] = reason
	}
	if message != "" {
		meta["handoff_message"] = message
	}
	if data, ok := payload["data"].(map[string]any); ok && len(data) > 0 {
		meta["handoff_data"] = data
	}
	if ts, ok := payload["ts_unix_ms"]; ok {
		meta["handoff_ts_unix_ms"] = ts
	}
	req := map[string]any{
		"directive":         directive,
		"append_to_session": true,
		"scope":             fmt.Sprintf("handoff:%s:%s", fromRole, toRole),
		"actor": map[string]any{
			"id":   cfg.orchestratorID,
			"kind": "orchestrator",
		},
		"metadata": meta,
		"targets": map[string]any{
			"roles": []string{toRole},
		},
	}
	url := fmt.Sprintf("%s/v1/teams/%s/runs/%s/moderator/directive", cfg.brokerBase, teamID, teamRunID)
	var resp moderatorDispatchResponse
	if err := doJSON(ctx, client, cfg, http.MethodPost, url, req, &resp); err != nil {
		return err
	}
	if !resp.OK {
		return fmt.Errorf("broker returned ok=false for handoff directive")
	}
	return nil
}

func isGuidanceDispatchRetriable(err error) bool {
	var herr *httpError
	if errors.As(err, &herr) {
		body := strings.ToLower(herr.Body)
		if strings.Contains(body, "no eligible members") {
			return true
		}
		if strings.Contains(body, "no eligible member sessions") {
			return true
		}
		if strings.Contains(body, "missing session_id") {
			return true
		}
	}
	return false
}
