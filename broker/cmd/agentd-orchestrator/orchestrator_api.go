package main

import (
	"context"
	"fmt"
	"net/http"
	"net/url"
	"strings"
)

func updateTeamRunGoalState(ctx context.Context, client *http.Client, cfg config, teamID, teamRunID string, goalContract map[string]any, event map[string]any) error {
	payload := map[string]any{}
	if goalContract != nil {
		payload["goal_contract"] = goalContract
	}
	if event != nil {
		payload["event"] = event
	}
	url := fmt.Sprintf("%s/v1/teams/%s/runs/%s/goal", cfg.brokerBase, teamID, teamRunID)
	var resp map[string]any
	if err := doJSON(ctx, client, cfg, http.MethodPatch, url, payload, &resp); err != nil {
		return err
	}
	return nil
}

func emitTeamRunGoalEvent(ctx context.Context, client *http.Client, cfg config, teamID, teamRunID string, event map[string]any) error {
	return updateTeamRunGoalState(ctx, client, cfg, teamID, teamRunID, nil, event)
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
