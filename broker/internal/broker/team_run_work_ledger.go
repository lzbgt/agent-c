package broker

import (
	"agentd-broker/internal/db"
	"agentd-broker/internal/workledger"
	"context"
	"fmt"
)

func manualWorkLedgerWaitMeta(meta map[string]any) map[string]any {
	next := cloneMap(meta)
	runtime, _ := meta["work_ledger_runtime"].(map[string]any)
	runtime = cloneMap(runtime)
	runtime["managed_wait"] = false
	runtime["reason"] = "owner_waiting"
	next["work_ledger_runtime"] = runtime
	return next
}

// Bound turns execute on one existing authority, not each member's local ledger.
func selectWorkLedgerAuthority(members []db.TeamMember, binding workledger.Binding) ([]db.TeamMember, error) {
	var selected []db.TeamMember
	for _, member := range members {
		if member.AgentID == binding.AgentID && member.DeploymentID == binding.DeploymentID {
			selected = append(selected, member)
		}
	}
	if len(selected) != 1 {
		return nil, fmt.Errorf("work ledger requires exactly one eligible team member on the bound agent and deployment")
	}
	return selected, nil
}

// Consume admission before host dispatch. Crashes never make this nonce reusable.
// This is fail-closed admission, not exactly-once host execution.
func (s *Server) admitWorkLedgerTurn(ctx context.Context, teamID string, teamMeta map[string]any, binding workledger.Binding) error {
	runID, ok := teamMeta["work_ledger_orchestrator_run_id"].(string)
	nonce, okNonce := teamMeta["work_ledger_dispatch_id"].(string)
	if !ok || !okNonce || !workledger.ValidID(runID) || !workledger.ValidID(nonce) {
		return fmt.Errorf("bound turn requires a goal dispatch admission")
	}
	run, err := s.cfg.DB.GetOrchestratorRun(ctx, teamID, runID)
	if err != nil {
		return fmt.Errorf("bound goal not found")
	}
	meta := run.Meta()
	current, err := workledger.ParseBinding(meta["work_ledger"])
	if err != nil || current != binding || run.Status != "running" {
		return fmt.Errorf("bound goal changed or is not running")
	}
	if enabled, ok := meta["autonomous"].(bool); ok && !enabled {
		return fmt.Errorf("bound goal autonomy is disabled")
	}
	if active, _ := meta["active_team_run_id"].(string); active != "" {
		return fmt.Errorf("bound goal already has an active team run")
	}
	runtime, ok := meta["work_ledger_runtime"].(map[string]any)
	if !ok {
		return fmt.Errorf("bound goal admission missing")
	}
	dispatch, ok := runtime["dispatch"].(map[string]any)
	if !ok || dispatch["id"] != nonce || dispatch["state"] != "admitted" {
		return fmt.Errorf("bound goal dispatch already consumed or missing")
	}
	if dispatch["control_digest"] != workledger.ControlDigest(run.Goal, run.GoalContract(), run.RolePlanSnapshot(), meta) {
		return fmt.Errorf("bound goal control changed after admission")
	}
	next := cloneMap(meta)
	nextRuntime := cloneMap(runtime)
	nextDispatch := cloneMap(dispatch)
	nextDispatch["state"] = "submitted"
	nextRuntime["dispatch"] = nextDispatch
	next["work_ledger_runtime"] = nextRuntime
	expected := run.Status
	_, err = s.cfg.DB.UpdateOrchestratorRun(ctx, teamID, runID, db.OrchestratorRunUpdate{Meta: next, ExpectedMeta: meta, ExpectedStatus: &expected})
	return err
}
