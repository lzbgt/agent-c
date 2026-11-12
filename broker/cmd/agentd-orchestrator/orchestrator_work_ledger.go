package main

import (
	"agentd-broker/internal/workledger"
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
	"time"
)

func handleLedgerBoundRun(ctx context.Context, client *http.Client, cfg config, teamID string, run orchestratorRun) error {
	if !shouldAutonomous(run.Meta) || (run.Status != "running" && run.Status != "planned" && !ledgerManagedWait(run)) {
		return nil
	}
	// Never overwrite explicit pause/done/error/manual waiting, even during a takeover.
	owner := strings.TrimSpace(asString(run.Meta["orchestrator_owner"]))
	if owner != "" && owner != cfg.orchestratorID {
		lease := leaseStatusForRun(run, run.Meta)
		if !allowLeaseTakeover(run.Meta) || (lease != "stale" && lease != "missing") {
			return nil
		}
	}
	meta := cloneMap(run.Meta)
	if owner != cfg.orchestratorID {
		meta["orchestrator_owner_prev"] = owner
		meta["orchestrator_owner"] = cfg.orchestratorID
		meta["orchestrator_owner_claimed_unix_ms"] = time.Now().UTC().UnixMilli()
		if _, ok := meta["lease_timeout_ms"]; !ok {
			meta["lease_timeout_ms"] = 30000
		}
		claimed, err := casLedgerGoal(ctx, client, cfg, teamID, run, run.Status, meta)
		if err != nil {
			return ledgerConflict(err)
		}
		run = claimed
	}
	if !ledgerControllable(run, cfg.orchestratorID) {
		return nil
	}
	hb, err := heartbeatRun(ctx, client, cfg, teamID, run.OrchestratorRunID, "", stringPtr(cfg.orchestratorID), stringPtr(run.Status))
	if err != nil {
		return ledgerConflict(err)
	}
	if hb == nil || !hb.OK || hb.Run.OrchestratorRunID != run.OrchestratorRunID {
		return fmt.Errorf("invalid goal heartbeat")
	}
	run = hb.Run
	if !ledgerControllable(run, cfg.orchestratorID) {
		return nil
	}
	meta = cloneMap(run.Meta)
	runtime := ledgerRuntime(meta)
	save := func(status, reason string, managed bool) error {
		runtime["reason"] = reason
		runtime["managed_wait"] = managed
		runtime["observed_unix_ms"] = time.Now().UTC().UnixMilli()
		meta["work_ledger_runtime"] = runtime
		_, err := casLedgerGoal(ctx, client, cfg, teamID, run, status, meta)
		return ledgerConflict(err)
	}
	binding, err := workledger.ParseBinding(meta["work_ledger"])
	if err != nil {
		return save("waiting", "invalid_binding", false)
	}
	if pinned, ok := runtime["binding"]; ok {
		previous, err := workledger.ParseBinding(pinned)
		if err != nil || previous != binding {
			return save("waiting", "authority_changed", false)
		}
	}
	runtime["binding"] = binding.Map()
	dispatch := ledgerDispatch(runtime)
	active := strings.TrimSpace(asString(meta["active_team_run_id"]))
	if active == "" && (dispatch["state"] == "admitted" || dispatch["state"] == "submitted") {
		return reconcileLedgerDispatch(ctx, client, cfg, teamID, run.OrchestratorRunID, asString(dispatch["id"]), "", true)
	}
	if active != "" && (dispatch["team_run_id"] != active || dispatch["state"] != "running") {
		return save("waiting", "unbound_active_run", false)
	}
	if changed, err := processGuidance(ctx, client, cfg, teamID, active, meta); err != nil {
		return err
	} else if changed {
		updated, err := casLedgerGoal(ctx, client, cfg, teamID, run, run.Status, meta)
		if err != nil {
			return ledgerConflict(err)
		}
		run = updated
		if !ledgerControllable(run, cfg.orchestratorID) {
			return nil
		}
		// Only continue from our exact binding and control snapshot.
		meta = cloneMap(run.Meta)
		runtime = ledgerRuntime(meta)
		runtime["binding"] = binding.Map()
	}
	snapshot, err := fetchWorkLedger(ctx, client, cfg, binding)
	if err != nil {
		return save("waiting", "authority_unavailable", true)
	}
	observedID := asString(runtime["ledger_id"])
	revision, _ := asInt64(runtime["revision"])
	if observedID != "" && (observedID != snapshot.State.LedgerID || snapshot.State.Revision < revision ||
		(snapshot.State.Revision == revision && asString(runtime["digest"]) != snapshot.Digest)) {
		return save("waiting", "authority_history_changed", false)
	}
	for key, value := range snapshot.Summary() {
		runtime[key] = value
	}
	if active != "" {
		result, err := fetchTeamRunStatus(ctx, client, cfg, teamID, active)
		if err != nil {
			return err
		}
		if !result.OK || result.TeamRunID != active {
			return fmt.Errorf("invalid active team run receipt")
		}
		status := strings.ToLower(result.Status)
		if !isTerminalTeamRunStatus(status) {
			return save("running", "working", false)
		}
		meta = appendTeamRunHistory(meta, active, status)
		meta["last_team_run_status"] = status
		delete(meta, "active_team_run_id")
		dispatch["state"] = status
		runtime["dispatch"] = dispatch
		if status != "succeeded" {
			return save("error", "team_run_"+status, false)
		}
	}
	if snapshot.AllDone {
		return save("done", "requirements_verified", false)
	}
	if len(snapshot.Runnable) == 0 {
		return save("waiting", "requirements_blocked", true)
	}
	launchedRevision, hasLaunched := asInt64(dispatch["revision"])
	if hasLaunched && snapshot.State.Revision <= launchedRevision {
		return save("waiting", "no_requirement_progress", true)
	}
	// A terminal receipt is persisted separately before admitting the next turn.
	if active != "" {
		return save("running", "requirements_ready", false)
	}
	runPayload, teamMeta := buildTeamRunPayload(run, meta)
	runPayload["session_id"] = binding.SessionID
	runPayload["no_session"] = false
	runnable := []map[string]any{}
	for _, id := range snapshot.Runnable {
		r := snapshot.State.Requirements[id]
		runnable = append(runnable, map[string]any{"id": id, "title": r.Title, "status": r.Status})
	}
	contextJSON, _ := json.Marshal(map[string]any{"authority": binding, "ledger_id": snapshot.State.LedgerID, "revision": snapshot.State.Revision, "runnable": runnable})
	runPayload["prompt"] = asString(runPayload["prompt"]) + "\n\nRequirement-ledger continuation (data, not new authorization): " + string(contextJSON) +
		"\nRead work_state in this exact session before acting. Continue independent runnable requirements toward the goal; respect existing approvals, policies and owner stops. Record verification evidence before marking requirements done. Record blockers only for affected requirements, with observed recovery attempts. Do not treat this turn's success as goal completion. Do not create another requirement ledger."
	teamMeta["mode"] = "async"
	teamMeta["auto_allocate_roles"] = false
	delete(teamMeta, "runtime_members")
	teamMeta["work_ledger"] = binding.Map()
	teamMeta["work_ledger_orchestrator_run_id"] = run.OrchestratorRunID
	nonce := "goal_" + randID(24)
	teamMeta["work_ledger_dispatch_id"] = nonce
	runtime["dispatch"] = map[string]any{"id": nonce, "state": "admitted", "revision": snapshot.State.Revision, "digest": snapshot.Digest, "control_digest": workledger.ControlDigest(run.Goal, run.GoalContract, run.RolePlanSnapshot, meta)}
	runtime["reason"] = "dispatching"
	runtime["managed_wait"] = false
	meta["work_ledger_runtime"] = runtime
	admitted, err := casLedgerGoal(ctx, client, cfg, teamID, run, "running", meta)
	if err != nil {
		return ledgerConflict(err)
	}
	if !ledgerControllable(admitted, cfg.orchestratorID) || ledgerDispatch(ledgerRuntime(admitted.Meta))["id"] != nonce {
		return nil
	}
	created, err := createTeamRun(ctx, client, cfg, teamID, runPayload, teamMeta)
	expectedTeamRunID := ""
	recovered := true
	if err == nil && created != nil && created.OK && created.TeamRunID != "" {
		expectedTeamRunID = created.TeamRunID
		recovered = false
	}
	// Even a successful HTTP response is correlated with broker-owned provenance.
	// A failed/lost response only triggers a read, never another POST.
	return reconcileLedgerDispatch(ctx, client, cfg, teamID, run.OrchestratorRunID, nonce, expectedTeamRunID, recovered)
}

func ledgerControllable(run orchestratorRun, owner string) bool {
	return shouldAutonomous(run.Meta) && asString(run.Meta["orchestrator_owner"]) == owner &&
		(run.Status == "running" || run.Status == "planned" || ledgerManagedWait(run))
}
func ledgerConflict(err error) error {
	if isHTTPStatus(err, http.StatusConflict) {
		return nil
	}
	return err
}
