package db

import (
	"context"
	"encoding/json"
	"errors"
	"strconv"
	"strings"
	"time"

	"github.com/jackc/pgx/v5"
)

type OrchestratorRun struct {
	OrchestratorRunID string
	TeamID            string
	Status            string
	Goal              string
	GoalContractJSON  json.RawMessage
	RolePlanJSON      json.RawMessage
	MetaJSON          json.RawMessage
	CreatedBy         string
	CreatedAt         time.Time
	UpdatedAt         time.Time
	LastHeartbeatAt   *time.Time
}

type OrchestratorRunUpdate struct {
	Status           *string
	Goal             *string
	GoalContract     map[string]any
	RolePlanSnapshot map[string]any
	Meta             map[string]any
}

func (r OrchestratorRun) GoalContract() map[string]any {
	return decodeMeta(r.GoalContractJSON)
}

func (r OrchestratorRun) RolePlanSnapshot() map[string]any {
	return decodeMeta(r.RolePlanJSON)
}

func (r OrchestratorRun) Meta() map[string]any {
	return decodeMeta(r.MetaJSON)
}

func (d *DB) CreateOrchestratorRun(
	ctx context.Context,
	orchestratorRunID,
	teamID,
	status,
	goal,
	createdBy string,
	goalContract,
	rolePlanSnapshot,
	meta map[string]any,
) (*OrchestratorRun, error) {
	if d == nil || d.Pool == nil {
		return nil, errors.New("db not open")
	}
	orchestratorRunID = strings.TrimSpace(orchestratorRunID)
	teamID = strings.TrimSpace(teamID)
	status = strings.TrimSpace(status)
	goal = strings.TrimSpace(goal)
	createdBy = strings.TrimSpace(createdBy)
	if orchestratorRunID == "" || teamID == "" {
		return nil, errors.New("missing orchestrator_run_id or team_id")
	}
	if status == "" {
		return nil, errors.New("missing status")
	}
	goalContractJSON, err := json.Marshal(coalesceMeta(goalContract))
	if err != nil {
		return nil, err
	}
	rolePlanJSON, err := json.Marshal(coalesceMeta(rolePlanSnapshot))
	if err != nil {
		return nil, err
	}
	metaJSON, err := json.Marshal(coalesceMeta(meta))
	if err != nil {
		return nil, err
	}
	_, err = d.Pool.Exec(ctx, `
		INSERT INTO broker_orchestrator_runs(
			orchestrator_run_id, team_id, status, goal, goal_contract, role_plan_snapshot, meta, created_by
		)
		VALUES($1, $2, $3, $4, $5::jsonb, $6::jsonb, $7::jsonb, $8)
	`, orchestratorRunID, teamID, status, nullIfEmpty(goal), string(goalContractJSON), string(rolePlanJSON), string(metaJSON), nullIfEmpty(createdBy))
	if err != nil {
		return nil, err
	}
	return d.GetOrchestratorRun(ctx, teamID, orchestratorRunID)
}

func (d *DB) GetOrchestratorRun(ctx context.Context, teamID, orchestratorRunID string) (*OrchestratorRun, error) {
	if d == nil || d.Pool == nil {
		return nil, errors.New("db not open")
	}
	teamID = strings.TrimSpace(teamID)
	orchestratorRunID = strings.TrimSpace(orchestratorRunID)
	if teamID == "" || orchestratorRunID == "" {
		return nil, errors.New("missing team_id or orchestrator_run_id")
	}
	var out OrchestratorRun
	err := d.Pool.QueryRow(ctx, `
		SELECT orchestrator_run_id, team_id, status, COALESCE(goal, ''), goal_contract::text,
		       role_plan_snapshot::text, meta::text, COALESCE(created_by, ''), created_at, updated_at, last_heartbeat_at
		FROM broker_orchestrator_runs
		WHERE team_id=$1 AND orchestrator_run_id=$2
	`, teamID, orchestratorRunID).Scan(
		&out.OrchestratorRunID,
		&out.TeamID,
		&out.Status,
		&out.Goal,
		&out.GoalContractJSON,
		&out.RolePlanJSON,
		&out.MetaJSON,
		&out.CreatedBy,
		&out.CreatedAt,
		&out.UpdatedAt,
		&out.LastHeartbeatAt,
	)
	if err != nil {
		return nil, err
	}
	return &out, nil
}

func (d *DB) ListOrchestratorRuns(ctx context.Context, teamID string, limit, offset int, status string) ([]OrchestratorRun, error) {
	if d == nil || d.Pool == nil {
		return nil, errors.New("db not open")
	}
	teamID = strings.TrimSpace(teamID)
	status = strings.TrimSpace(status)
	if teamID == "" {
		return nil, errors.New("missing team_id")
	}
	if limit <= 0 {
		limit = 25
	}
	if limit > 200 {
		limit = 200
	}
	if offset < 0 {
		offset = 0
	}
	var rows pgx.Rows
	var err error
	if status == "" {
		rows, err = d.Pool.Query(ctx, `
			SELECT orchestrator_run_id, team_id, status, COALESCE(goal, ''), goal_contract::text,
			       role_plan_snapshot::text, meta::text, COALESCE(created_by, ''), created_at, updated_at, last_heartbeat_at
			FROM broker_orchestrator_runs
			WHERE team_id=$1
			ORDER BY created_at DESC, orchestrator_run_id DESC
			LIMIT $2 OFFSET $3
		`, teamID, limit, offset)
	} else {
		rows, err = d.Pool.Query(ctx, `
			SELECT orchestrator_run_id, team_id, status, COALESCE(goal, ''), goal_contract::text,
			       role_plan_snapshot::text, meta::text, COALESCE(created_by, ''), created_at, updated_at, last_heartbeat_at
			FROM broker_orchestrator_runs
			WHERE team_id=$1 AND status=$2
			ORDER BY created_at DESC, orchestrator_run_id DESC
			LIMIT $3 OFFSET $4
		`, teamID, status, limit, offset)
	}
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []OrchestratorRun{}
	for rows.Next() {
		var r OrchestratorRun
		if err := rows.Scan(
			&r.OrchestratorRunID,
			&r.TeamID,
			&r.Status,
			&r.Goal,
			&r.GoalContractJSON,
			&r.RolePlanJSON,
			&r.MetaJSON,
			&r.CreatedBy,
			&r.CreatedAt,
			&r.UpdatedAt,
			&r.LastHeartbeatAt,
		); err != nil {
			return nil, err
		}
		out = append(out, r)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	return out, nil
}

func (d *DB) UpdateOrchestratorRun(ctx context.Context, teamID, orchestratorRunID string, update OrchestratorRunUpdate) (*OrchestratorRun, error) {
	if d == nil || d.Pool == nil {
		return nil, errors.New("db not open")
	}
	teamID = strings.TrimSpace(teamID)
	orchestratorRunID = strings.TrimSpace(orchestratorRunID)
	if teamID == "" || orchestratorRunID == "" {
		return nil, errors.New("missing team_id or orchestrator_run_id")
	}
	sets := []string{}
	args := []any{}
	push := func(sql string, val any) {
		args = append(args, val)
		sets = append(sets, sql)
	}
	if update.Status != nil {
		status := strings.TrimSpace(*update.Status)
		if status == "" {
			return nil, errors.New("missing status")
		}
		push("status=$"+strconv.Itoa(len(args)+1), status)
	}
	if update.Goal != nil {
		goal := strings.TrimSpace(*update.Goal)
		push("goal=$"+strconv.Itoa(len(args)+1), nullIfEmpty(goal))
	}
	if update.GoalContract != nil {
		b, err := json.Marshal(coalesceMeta(update.GoalContract))
		if err != nil {
			return nil, err
		}
		push("goal_contract=$"+strconv.Itoa(len(args)+1)+"::jsonb", string(b))
	}
	if update.RolePlanSnapshot != nil {
		b, err := json.Marshal(coalesceMeta(update.RolePlanSnapshot))
		if err != nil {
			return nil, err
		}
		push("role_plan_snapshot=$"+strconv.Itoa(len(args)+1)+"::jsonb", string(b))
	}
	if update.Meta != nil {
		b, err := json.Marshal(coalesceMeta(update.Meta))
		if err != nil {
			return nil, err
		}
		push("meta=$"+strconv.Itoa(len(args)+1)+"::jsonb", string(b))
	}
	if len(sets) == 0 {
		return nil, errors.New("no updates")
	}
	sets = append(sets, "updated_at=NOW()")
	args = append(args, teamID, orchestratorRunID)
	query := "UPDATE broker_orchestrator_runs SET " + strings.Join(sets, ", ") + " WHERE team_id=$" + strconv.Itoa(len(args)-1) + " AND orchestrator_run_id=$" + strconv.Itoa(len(args))
	if _, err := d.Pool.Exec(ctx, query, args...); err != nil {
		return nil, err
	}
	return d.GetOrchestratorRun(ctx, teamID, orchestratorRunID)
}

func (d *DB) UpdateOrchestratorRunHeartbeat(ctx context.Context, teamID, orchestratorRunID string, status *string) (*OrchestratorRun, error) {
	if d == nil || d.Pool == nil {
		return nil, errors.New("db not open")
	}
	teamID = strings.TrimSpace(teamID)
	orchestratorRunID = strings.TrimSpace(orchestratorRunID)
	if teamID == "" || orchestratorRunID == "" {
		return nil, errors.New("missing team_id or orchestrator_run_id")
	}
	args := []any{}
	sets := []string{"last_heartbeat_at=NOW()", "updated_at=NOW()"}
	if status != nil {
		statusTrim := strings.TrimSpace(*status)
		if statusTrim == "" {
			return nil, errors.New("missing status")
		}
		args = append(args, statusTrim)
		sets = append(sets, "status=$"+strconv.Itoa(len(args)))
	}
	args = append(args, teamID, orchestratorRunID)
	query := "UPDATE broker_orchestrator_runs SET " + strings.Join(sets, ", ") + " WHERE team_id=$" + strconv.Itoa(len(args)-1) + " AND orchestrator_run_id=$" + strconv.Itoa(len(args))
	if _, err := d.Pool.Exec(ctx, query, args...); err != nil {
		return nil, err
	}
	return d.GetOrchestratorRun(ctx, teamID, orchestratorRunID)
}

func coalesceMeta(v map[string]any) map[string]any {
	if v == nil {
		return map[string]any{}
	}
	return v
}
