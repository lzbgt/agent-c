package main

import (
	"agentd-broker/internal/workledger"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"time"
)

var errLedgerReceiptMismatch = errors.New("dispatch receipt identity mismatch")

func fetchLedgerDispatch(ctx context.Context, client *http.Client, cfg config, teamID, runID, nonce string) (*workledger.DispatchReceipt, error) {
	if !workledger.ValidID(teamID) || !workledger.ValidID(runID) || !workledger.ValidID(nonce) {
		return nil, errLedgerReceiptMismatch
	}
	endpoint := cfg.brokerBase + "/v1/teams/" + url.PathEscape(teamID) + "/orchestrator/runs/" + url.PathEscape(runID) + "/dispatches/" + url.PathEscape(nonce)
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
	guarded := *client
	guarded.CheckRedirect = func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }
	response, err := guarded.Do(req)
	if err != nil {
		return nil, fmt.Errorf("dispatch receipt unavailable")
	}
	defer response.Body.Close()
	if response.StatusCode == http.StatusNotFound {
		return nil, nil
	}
	if response.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("dispatch receipt returned HTTP %d", response.StatusCode)
	}
	const max = 64 * 1024
	data, err := io.ReadAll(io.LimitReader(response.Body, max+1))
	if err != nil {
		return nil, fmt.Errorf("dispatch receipt read failed")
	}
	if len(data) > max {
		return nil, errLedgerReceiptMismatch
	}
	var payload struct {
		OK       bool                       `json:"ok"`
		Dispatch workledger.DispatchReceipt `json:"dispatch"`
	}
	if err := json.Unmarshal(data, &payload); err != nil || !payload.OK || payload.Dispatch.Validate() != nil ||
		payload.Dispatch.TeamID != teamID || payload.Dispatch.OrchestratorRunID != runID || payload.Dispatch.DispatchID != nonce {
		return nil, errLedgerReceiptMismatch
	}
	return &payload.Dispatch, nil
}

func receiptMatchesGoal(receipt *workledger.DispatchReceipt, current orchestratorRun) bool {
	if receipt == nil || receipt.Validate() != nil || receipt.TeamID != current.TeamID || receipt.OrchestratorRunID != current.OrchestratorRunID {
		return false
	}
	runtime := ledgerRuntime(current.Meta)
	dispatch := ledgerDispatch(runtime)
	binding, err := workledger.ParseBinding(current.Meta["work_ledger"])
	pinned, pinErr := workledger.ParseBinding(runtime["binding"])
	revision, ok := asInt64(dispatch["revision"])
	return err == nil && pinErr == nil && binding == receipt.Binding && pinned == binding &&
		dispatch["id"] == receipt.DispatchID && runtime["ledger_id"] == receipt.LedgerID &&
		ok && revision == receipt.Revision && dispatch["digest"] == receipt.Digest && dispatch["control_digest"] == receipt.ControlDigest
}

func reconcileLedgerDispatch(ctx context.Context, client *http.Client, cfg config, teamID, runID, nonce, expectedTeamRunID string, recovered bool) error {
	receipt, err := fetchLedgerDispatch(ctx, client, cfg, teamID, runID, nonce)
	if errors.Is(err, errLedgerReceiptMismatch) {
		return waitLedgerDispatch(ctx, client, cfg, teamID, runID, nonce, "dispatch_receipt_mismatch", false)
	}
	if err != nil {
		return waitLedgerDispatch(ctx, client, cfg, teamID, runID, nonce, "dispatch_receipt_unavailable", true)
	}
	if receipt == nil {
		return waitLedgerDispatch(ctx, client, cfg, teamID, runID, nonce, "dispatch_receipt_pending", true)
	}
	if expectedTeamRunID != "" && (receipt.State != "recorded" || receipt.TeamRunID != expectedTeamRunID) {
		return waitLedgerDispatch(ctx, client, cfg, teamID, runID, nonce, "dispatch_receipt_mismatch", false)
	}
	if receipt.State != "recorded" {
		// Admission alone proves neither delivery nor absence. It never permits a retry.
		current, err := getLedgerGoal(ctx, client, cfg, teamID, runID)
		if err != nil {
			return err
		}
		if !receiptMatchesGoal(receipt, current) {
			return waitLedgerDispatch(ctx, client, cfg, teamID, runID, nonce, "dispatch_receipt_mismatch", false)
		}
		return waitLedgerDispatch(ctx, client, cfg, teamID, runID, nonce, "dispatch_receipt_pending", true)
	}
	err = recordLedgerDispatch(ctx, client, cfg, receipt, recovered)
	if errors.Is(err, errLedgerReceiptMismatch) {
		return waitLedgerDispatch(ctx, client, cfg, teamID, runID, nonce, "dispatch_receipt_mismatch", false)
	}
	return err
}

// Only a still-owned, controllable goal can enter an automatic read-only wait.
// Always refresh first: broker admission or an owner edit may have changed meta.
func waitLedgerDispatch(ctx context.Context, client *http.Client, cfg config, teamID, runID, nonce, reason string, managed bool) error {
	for attempt := 0; attempt < 2; attempt++ {
		current, err := getLedgerGoal(ctx, client, cfg, teamID, runID)
		if err != nil {
			return err
		}
		runtime := ledgerRuntime(current.Meta)
		if !ledgerControllable(current, cfg.orchestratorID) || ledgerDispatch(runtime)["id"] != nonce || asString(current.Meta["active_team_run_id"]) != "" {
			return nil
		}
		meta := cloneMap(current.Meta)
		runtime["reason"] = reason
		runtime["managed_wait"] = managed
		runtime["observed_unix_ms"] = time.Now().UTC().UnixMilli()
		meta["work_ledger_runtime"] = runtime
		_, err = casLedgerGoal(ctx, client, cfg, teamID, current, "waiting", meta)
		if isHTTPStatus(err, http.StatusConflict) {
			continue
		}
		return err
	}
	return fmt.Errorf("dispatch receipt wait changed concurrently; refresh before continuing")
}

// A receipt only attaches the original run. It cannot complete requirements,
// replace an admission, change authority, or unpause/reenable an owner's goal.
func recordLedgerDispatch(ctx context.Context, client *http.Client, cfg config, receipt *workledger.DispatchReceipt, recovered bool) error {
	for attempt := 0; attempt < 2; attempt++ {
		current, err := getLedgerGoal(ctx, client, cfg, receipt.TeamID, receipt.OrchestratorRunID)
		if err != nil {
			return err
		}
		if !receiptMatchesGoal(receipt, current) {
			return errLedgerReceiptMismatch
		}
		meta := cloneMap(current.Meta)
		runtime := ledgerRuntime(meta)
		dispatch := ledgerDispatch(runtime)
		active := asString(meta["active_team_run_id"])
		if active != "" && active != receipt.TeamRunID {
			return errLedgerReceiptMismatch
		}
		if active == receipt.TeamRunID && dispatch["state"] == "running" {
			return nil
		}
		if dispatch["state"] != "admitted" && dispatch["state"] != "submitted" {
			return errLedgerReceiptMismatch
		}
		dispatch["state"] = "running"
		dispatch["team_run_id"] = receipt.TeamRunID
		dispatch["receipt_recorded_unix_ms"] = receipt.RecordedUnixMS
		dispatch["receipt_recovered"] = recovered
		runtime["dispatch"] = dispatch
		status := current.Status
		if ledgerControllable(current, cfg.orchestratorID) {
			runtime["reason"] = "working"
			runtime["managed_wait"] = false
			if ledgerManagedWait(current) {
				status = "running"
			}
		}
		meta["work_ledger_runtime"] = runtime
		meta["active_team_run_id"] = receipt.TeamRunID
		_, err = casLedgerGoal(ctx, client, cfg, receipt.TeamID, current, status, meta)
		if isHTTPStatus(err, http.StatusConflict) {
			continue
		}
		return err
	}
	return fmt.Errorf("dispatch receipt needs reconciliation after concurrent goal edits")
}
