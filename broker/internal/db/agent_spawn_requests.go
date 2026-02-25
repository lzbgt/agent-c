package db

import (
	"context"
	"encoding/json"
	"errors"
	"strconv"
	"strings"
	"time"
)

type AgentSpawnRequest struct {
	SpawnRequestID      string
	TeamID              string
	OrchestratorRunID   string
	Role                string
	Count               int
	Status              string
	RequirementsJSON    json.RawMessage
	AssignedMembersJSON json.RawMessage
	ErrorText           string
	MetaJSON            json.RawMessage
	CreatedBy           string
	CreatedAt           time.Time
	UpdatedAt           time.Time
}

type AgentSpawnRequestUpdate struct {
	Status          *string
	ExpectedStatus  *string
	Requirements    map[string]any
	AssignedMembers *[]map[string]any
	Error           *string
	Meta            map[string]any
}

var ErrAgentSpawnRequestConflict = errors.New("spawn request status mismatch")

func (r AgentSpawnRequest) Requirements() map[string]any {
	return decodeMeta(r.RequirementsJSON)
}

func (r AgentSpawnRequest) AssignedMembers() []map[string]any {
	return decodeMetaSlice(r.AssignedMembersJSON)
}

func (r AgentSpawnRequest) Meta() map[string]any {
	return decodeMeta(r.MetaJSON)
}

func (d *DB) CreateAgentSpawnRequest(
	ctx context.Context,
	spawnRequestID,
	teamID,
	orchestratorRunID,
	role string,
	count int,
	status string,
	createdBy string,
	requirements map[string]any,
	assignedMembers []map[string]any,
	meta map[string]any,
) (*AgentSpawnRequest, error) {
	if d == nil || d.Pool == nil {
		return nil, errors.New("db not open")
	}
	spawnRequestID = strings.TrimSpace(spawnRequestID)
	teamID = strings.TrimSpace(teamID)
	orchestratorRunID = strings.TrimSpace(orchestratorRunID)
	role = strings.TrimSpace(role)
	status = strings.TrimSpace(status)
	createdBy = strings.TrimSpace(createdBy)
	if spawnRequestID == "" || teamID == "" {
		return nil, errors.New("missing spawn_request_id or team_id")
	}
	if role == "" {
		return nil, errors.New("missing role")
	}
	if status == "" {
		return nil, errors.New("missing status")
	}
	if count <= 0 {
		return nil, errors.New("count must be > 0")
	}
	reqJSON, err := json.Marshal(coalesceMeta(requirements))
	if err != nil {
		return nil, err
	}
	assignedJSON, err := json.Marshal(coalesceMemberList(assignedMembers))
	if err != nil {
		return nil, err
	}
	metaJSON, err := json.Marshal(coalesceMeta(meta))
	if err != nil {
		return nil, err
	}
	_, err = d.Pool.Exec(ctx, `
		INSERT INTO broker_agent_spawn_requests(
			spawn_request_id, team_id, orchestrator_run_id, role, count, status,
			requirements, assigned_members, error, meta, created_by
		)
		VALUES($1, $2, $3, $4, $5, $6, $7::jsonb, $8::jsonb, $9, $10::jsonb, $11)
	`, spawnRequestID, teamID, nullIfEmpty(orchestratorRunID), role, count, status, string(reqJSON), string(assignedJSON), nil, string(metaJSON), nullIfEmpty(createdBy))
	if err != nil {
		return nil, err
	}
	return d.GetAgentSpawnRequest(ctx, teamID, spawnRequestID)
}

func (d *DB) GetAgentSpawnRequest(ctx context.Context, teamID, spawnRequestID string) (*AgentSpawnRequest, error) {
	if d == nil || d.Pool == nil {
		return nil, errors.New("db not open")
	}
	teamID = strings.TrimSpace(teamID)
	spawnRequestID = strings.TrimSpace(spawnRequestID)
	if teamID == "" || spawnRequestID == "" {
		return nil, errors.New("missing team_id or spawn_request_id")
	}
	var out AgentSpawnRequest
	var orchestratorRunID *string
	var errorText *string
	err := d.Pool.QueryRow(ctx, `
		SELECT spawn_request_id, team_id, orchestrator_run_id, role, count, status,
		       requirements::text, assigned_members::text, error, meta::text,
		       COALESCE(created_by, ''), created_at, updated_at
		FROM broker_agent_spawn_requests
		WHERE team_id=$1 AND spawn_request_id=$2
	`, teamID, spawnRequestID).Scan(
		&out.SpawnRequestID,
		&out.TeamID,
		&orchestratorRunID,
		&out.Role,
		&out.Count,
		&out.Status,
		&out.RequirementsJSON,
		&out.AssignedMembersJSON,
		&errorText,
		&out.MetaJSON,
		&out.CreatedBy,
		&out.CreatedAt,
		&out.UpdatedAt,
	)
	if err != nil {
		return nil, err
	}
	if orchestratorRunID != nil {
		out.OrchestratorRunID = *orchestratorRunID
	}
	if errorText != nil {
		out.ErrorText = *errorText
	}
	return &out, nil
}

func (d *DB) ListAgentSpawnRequests(ctx context.Context, teamID string, limit, offset int, status, orchestratorRunID string) ([]AgentSpawnRequest, error) {
	if d == nil || d.Pool == nil {
		return nil, errors.New("db not open")
	}
	teamID = strings.TrimSpace(teamID)
	status = strings.TrimSpace(status)
	orchestratorRunID = strings.TrimSpace(orchestratorRunID)
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
	query := `
		SELECT spawn_request_id, team_id, orchestrator_run_id, role, count, status,
		       requirements::text, assigned_members::text, error, meta::text,
		       COALESCE(created_by, ''), created_at, updated_at
		FROM broker_agent_spawn_requests
		WHERE team_id=$1`
	args := []any{teamID}
	if status != "" {
		args = append(args, status)
		query += " AND status=$" + strconv.Itoa(len(args))
	}
	if orchestratorRunID != "" {
		args = append(args, orchestratorRunID)
		query += " AND orchestrator_run_id=$" + strconv.Itoa(len(args))
	}
	query += " ORDER BY created_at DESC, spawn_request_id DESC LIMIT $" + strconv.Itoa(len(args)+1) + " OFFSET $" + strconv.Itoa(len(args)+2)
	args = append(args, limit, offset)
	rows, err := d.Pool.Query(ctx, query, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []AgentSpawnRequest{}
	for rows.Next() {
		var r AgentSpawnRequest
		var orchestratorRunIDRow *string
		var errorText *string
		if err := rows.Scan(
			&r.SpawnRequestID,
			&r.TeamID,
			&orchestratorRunIDRow,
			&r.Role,
			&r.Count,
			&r.Status,
			&r.RequirementsJSON,
			&r.AssignedMembersJSON,
			&errorText,
			&r.MetaJSON,
			&r.CreatedBy,
			&r.CreatedAt,
			&r.UpdatedAt,
		); err != nil {
			return nil, err
		}
		if orchestratorRunIDRow != nil {
			r.OrchestratorRunID = *orchestratorRunIDRow
		}
		if errorText != nil {
			r.ErrorText = *errorText
		}
		out = append(out, r)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	return out, nil
}

func (d *DB) UpdateAgentSpawnRequest(ctx context.Context, teamID, spawnRequestID string, update AgentSpawnRequestUpdate) (*AgentSpawnRequest, error) {
	if d == nil || d.Pool == nil {
		return nil, errors.New("db not open")
	}
	teamID = strings.TrimSpace(teamID)
	spawnRequestID = strings.TrimSpace(spawnRequestID)
	if teamID == "" || spawnRequestID == "" {
		return nil, errors.New("missing team_id or spawn_request_id")
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
	if update.Requirements != nil {
		b, err := json.Marshal(coalesceMeta(update.Requirements))
		if err != nil {
			return nil, err
		}
		push("requirements=$"+strconv.Itoa(len(args)+1)+"::jsonb", string(b))
	}
	if update.AssignedMembers != nil {
		b, err := json.Marshal(coalesceMemberList(*update.AssignedMembers))
		if err != nil {
			return nil, err
		}
		push("assigned_members=$"+strconv.Itoa(len(args)+1)+"::jsonb", string(b))
	}
	if update.Error != nil {
		push("error=$"+strconv.Itoa(len(args)+1), nullIfEmpty(*update.Error))
	}
	if update.Meta != nil {
		b, err := json.Marshal(coalesceMeta(update.Meta))
		if err != nil {
			return nil, err
		}
		push("meta=$"+strconv.Itoa(len(args)+1)+"::jsonb", string(b))
	}
	var expectedStatus string
	if update.ExpectedStatus != nil {
		expectedStatus = strings.TrimSpace(*update.ExpectedStatus)
		if expectedStatus == "" {
			return nil, errors.New("missing expected_status")
		}
	}
	if len(sets) == 0 {
		return nil, errors.New("no updates")
	}
	sets = append(sets, "updated_at=NOW()")
	args = append(args, teamID, spawnRequestID)
	query := "UPDATE broker_agent_spawn_requests SET " + strings.Join(sets, ", ") +
		" WHERE team_id=$" + strconv.Itoa(len(args)-1) +
		" AND spawn_request_id=$" + strconv.Itoa(len(args))
	if expectedStatus != "" {
		args = append(args, expectedStatus)
		query += " AND status=$" + strconv.Itoa(len(args))
	}
	tag, err := d.Pool.Exec(ctx, query, args...)
	if err != nil {
		return nil, err
	}
	if expectedStatus != "" && tag.RowsAffected() == 0 {
		return nil, ErrAgentSpawnRequestConflict
	}
	return d.GetAgentSpawnRequest(ctx, teamID, spawnRequestID)
}

func decodeMetaSlice(raw json.RawMessage) []map[string]any {
	if len(raw) == 0 {
		return []map[string]any{}
	}
	var out []map[string]any
	if err := json.Unmarshal(raw, &out); err != nil {
		return []map[string]any{}
	}
	if out == nil {
		out = []map[string]any{}
	}
	return out
}

func coalesceMemberList(v []map[string]any) []map[string]any {
	if v == nil {
		return []map[string]any{}
	}
	return v
}
