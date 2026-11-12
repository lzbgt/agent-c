package db

import (
	"context"
	"encoding/json"
	"errors"
	"time"

	"agentd-broker/internal/workledger"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
)

var ErrGoalDispatchConflict = errors.New("goal dispatch admission or receipt conflict")

// Admission and the complete control CAS commit together before any host call.
// The unique key also prevents reuse after mutable goal metadata is reset.
func (d *DB) AdmitGoalDispatch(ctx context.Context, expected *OrchestratorRun, nextMeta map[string]any, receipt workledger.DispatchReceipt) (*workledger.DispatchReceipt, error) {
	if expected == nil || expected.Status != "running" || expected.TeamID != receipt.TeamID || expected.OrchestratorRunID != receipt.OrchestratorRunID {
		return nil, ErrGoalDispatchConflict
	}
	if err := receipt.ValidateIdentity(); err != nil {
		return nil, err
	}
	if receipt.State != "admitted" || receipt.TeamRunID != "" || receipt.RecordedUnixMS != 0 {
		return nil, ErrGoalDispatchConflict
	}
	next, err := json.Marshal(nextMeta)
	if err != nil {
		return nil, err
	}
	tx, err := d.Pool.Begin(ctx)
	if err != nil {
		return nil, err
	}
	defer tx.Rollback(ctx)
	tag, err := tx.Exec(ctx, `
 UPDATE broker_orchestrator_runs SET meta=$1::jsonb, updated_at=NOW()
 WHERE team_id=$2 AND orchestrator_run_id=$3 AND status='running'
 AND meta=$4::jsonb AND COALESCE(goal,'')=$5
 AND goal_contract=$6::jsonb AND role_plan_snapshot=$7::jsonb
 `, string(next), expected.TeamID, expected.OrchestratorRunID, string(expected.MetaJSON), expected.Goal,
		string(expected.GoalContractJSON), string(expected.RolePlanJSON))
	if err != nil {
		return nil, err
	}
	if tag.RowsAffected() != 1 {
		return nil, ErrGoalDispatchConflict
	}
	var admitted time.Time
	err = tx.QueryRow(ctx, `
 INSERT INTO broker_goal_dispatches(team_id,orchestrator_run_id,dispatch_id,agent_id,deployment_id,session_id,ledger_id,revision,digest,control_digest)
 VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10) RETURNING admitted_at
 `, receipt.TeamID, receipt.OrchestratorRunID, receipt.DispatchID, receipt.Binding.AgentID, receipt.Binding.DeploymentID,
		receipt.Binding.SessionID, receipt.LedgerID, receipt.Revision, receipt.Digest, receipt.ControlDigest).Scan(&admitted)
	if err != nil {
		var pgerr *pgconn.PgError
		if errors.As(err, &pgerr) && pgerr.Code == "23505" {
			return nil, ErrGoalDispatchConflict
		}
		return nil, err
	}
	if err := tx.Commit(ctx); err != nil {
		return nil, err
	}
	receipt.AdmittedUnixMS = admitted.UnixMilli()
	return &receipt, nil
}

// Store the team row and trusted receipt in one transaction. A lost HTTP reply
// can be recovered by GET; a crash before commit remains explicitly uncertain.
// There is deliberately no upsert, overwrite, or host redispatch here.
func (d *DB) CreateTeamRunWithGoalReceipt(ctx context.Context, teamRunID, teamID, status, createdBy string, runJSON []byte, admission *workledger.DispatchReceipt) (*TeamRun, error) {
	if admission == nil || admission.TeamID != teamID || admission.State != "admitted" || admission.TeamRunID != "" || !workledger.ValidID(teamRunID) {
		return nil, ErrGoalDispatchConflict
	}
	if err := admission.Validate(); err != nil {
		return nil, err
	}
	tx, err := d.Pool.Begin(ctx)
	if err != nil {
		return nil, err
	}
	defer tx.Rollback(ctx)
	var run TeamRun
	var payload string
	err = tx.QueryRow(ctx, `
 INSERT INTO broker_team_runs(team_run_id,team_id,status,created_by,run_json)
 VALUES($1,$2,$3,$4,$5::jsonb)
 RETURNING team_run_id,team_id,status,COALESCE(created_by,''),run_json::text,created_at
 `, teamRunID, teamID, status, nullIfEmpty(createdBy), string(runJSON)).Scan(
		&run.TeamRunID, &run.TeamID, &run.Status, &run.CreatedBy, &payload, &run.CreatedAt)
	if err != nil {
		return nil, err
	}
	tag, err := tx.Exec(ctx, `
 UPDATE broker_goal_dispatches SET team_run_id=$1, recorded_at=NOW()
 WHERE team_id=$2 AND orchestrator_run_id=$3 AND dispatch_id=$4 AND team_run_id IS NULL
 AND agent_id=$5 AND deployment_id=$6 AND session_id=$7 AND ledger_id=$8
 AND revision=$9 AND digest=$10 AND control_digest=$11
 `, teamRunID, teamID, admission.OrchestratorRunID, admission.DispatchID, admission.Binding.AgentID,
		admission.Binding.DeploymentID, admission.Binding.SessionID, admission.LedgerID, admission.Revision,
		admission.Digest, admission.ControlDigest)
	if err != nil {
		return nil, err
	}
	if tag.RowsAffected() != 1 {
		return nil, ErrGoalDispatchConflict
	}
	if err = tx.Commit(ctx); err != nil {
		return nil, err
	}
	run.RunJSON = json.RawMessage(payload)
	return &run, nil
}

func (d *DB) GetGoalDispatch(ctx context.Context, teamID, runID, nonce string) (*workledger.DispatchReceipt, error) {
	var receipt workledger.DispatchReceipt
	var admitted time.Time
	var recorded *time.Time
	err := d.Pool.QueryRow(ctx, `
 SELECT team_id,orchestrator_run_id,dispatch_id,agent_id,deployment_id,session_id,ledger_id,revision,digest,control_digest,
 COALESCE(team_run_id,''),admitted_at,recorded_at
 FROM broker_goal_dispatches WHERE team_id=$1 AND orchestrator_run_id=$2 AND dispatch_id=$3
 `, teamID, runID, nonce).Scan(&receipt.TeamID, &receipt.OrchestratorRunID, &receipt.DispatchID,
		&receipt.Binding.AgentID, &receipt.Binding.DeploymentID, &receipt.Binding.SessionID, &receipt.LedgerID, &receipt.Revision,
		&receipt.Digest, &receipt.ControlDigest, &receipt.TeamRunID, &admitted, &recorded)
	if err != nil {
		return nil, err
	}
	receipt.Schema = "agentd.goal-dispatch.v1"
	receipt.State = "admitted"
	receipt.AdmittedUnixMS = admitted.UnixMilli()
	if recorded != nil {
		receipt.State = "recorded"
		receipt.RecordedUnixMS = recorded.UnixMilli()
	}
	if err := receipt.Validate(); err != nil {
		return nil, err
	}
	return &receipt, nil
}

// Keep not-found handling at the HTTP boundary, not a fallback to metadata scans.
func IsGoalDispatchMissing(err error) bool { return errors.Is(err, pgx.ErrNoRows) }
