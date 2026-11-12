package main

import (
	"agentd-broker/internal/workledger"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
)

// Only the fixed authenticated proxy path is accepted. No redirect or arbitrary
// URL/file source can substitute an authority; body size is bounded before decoding.
func fetchWorkLedger(ctx context.Context, client *http.Client, cfg config, b workledger.Binding) (*workledger.Snapshot, error) {
	endpoint := cfg.brokerBase + "/v1/agents/" + url.PathEscape(b.AgentID) + "/proxy/api/v1/session/work?session_id=" + url.QueryEscape(b.SessionID)
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint, nil)
	if err != nil {
		return nil, err
	}
	token, err := cfg.bearerToken()
	if err != nil {
		return nil, err
	}
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Accept", "application/json")
	req.Header.Set("X-Agentd-Deployment", b.DeploymentID)
	guarded := *client
	guarded.CheckRedirect = func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }
	response, err := guarded.Do(req)
	if err != nil {
		return nil, fmt.Errorf("ledger authority unavailable")
	}
	defer response.Body.Close()
	const max = 2 * 1024 * 1024
	data, err := io.ReadAll(io.LimitReader(response.Body, max+1))
	if err != nil || len(data) > max {
		return nil, fmt.Errorf("ledger response unavailable or exceeds 2 MiB")
	}
	if response.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("ledger authority returned HTTP %d", response.StatusCode)
	}
	var snapshot workledger.Snapshot
	if err = json.Unmarshal(data, &snapshot); err != nil {
		return nil, fmt.Errorf("invalid ledger response")
	}
	if err = snapshot.Validate(b); err != nil {
		return nil, err
	}
	return &snapshot, nil
}

func getLedgerGoal(ctx context.Context, client *http.Client, cfg config, teamID, runID string) (orchestratorRun, error) {
	var response orchestratorRunResponse
	err := doJSON(ctx, client, cfg, http.MethodGet, fmt.Sprintf("%s/v1/teams/%s/orchestrator/runs/%s", cfg.brokerBase, teamID, runID), nil, &response)
	if err == nil && (!response.OK || response.Run.OrchestratorRunID != runID) {
		err = fmt.Errorf("invalid goal response")
	}
	return response.Run, err
}

// Exact metadata and status, including owner and binding, fence every transition.
func casLedgerGoal(ctx context.Context, client *http.Client, cfg config, teamID string, prev orchestratorRun, status string, meta map[string]any) (orchestratorRun, error) {
	payload := map[string]any{"status": status, "meta": meta, "expected_status": prev.Status, "expected_meta": cloneMap(prev.Meta), "expected_owner": asString(prev.Meta["orchestrator_owner"])}
	var response orchestratorRunResponse
	err := doJSON(ctx, client, cfg, http.MethodPatch, fmt.Sprintf("%s/v1/teams/%s/orchestrator/runs/%s", cfg.brokerBase, teamID, prev.OrchestratorRunID), payload, &response)
	if err == nil && (!response.OK || response.Run.OrchestratorRunID != prev.OrchestratorRunID) {
		err = fmt.Errorf("invalid goal transition response")
	}
	if err == nil {
		want, _ := json.Marshal(meta)
		got, _ := json.Marshal(response.Run.Meta)
		if response.Run.Status != status || string(want) != string(got) {
			err = fmt.Errorf("goal changed after transition; refresh before continuing")
		}
	}
	return response.Run, err
}
func ledgerRuntime(meta map[string]any) map[string]any {
	v, _ := meta["work_ledger_runtime"].(map[string]any)
	return cloneMap(v)
}
func ledgerDispatch(runtime map[string]any) map[string]any {
	v, _ := runtime["dispatch"].(map[string]any)
	return cloneMap(v)
}
func ledgerManagedWait(run orchestratorRun) bool {
	_, bound := run.Meta["work_ledger"]
	runtime := ledgerRuntime(run.Meta)
	managed, _ := runtime["managed_wait"].(bool)
	return bound && run.Status == "waiting" && managed
}
