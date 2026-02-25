package db

import (
	"context"
	"encoding/json"
	"errors"
	"strings"
	"time"

	"github.com/jackc/pgx/v5"
)

type GuidanceEvent struct {
	GuidanceID          string
	TeamID              string
	TeamRunID           string
	Kind                string
	Priority            string
	Message             string
	PayloadJSON         json.RawMessage
	TargetRolesJSON     json.RawMessage
	TargetMemberIDsJSON json.RawMessage
	TargetAgentIDsJSON  json.RawMessage
	TargetOrchestrator  string
	CreatedBy           string
	CreatedSub          string
	CreatedUnixMS       int64
	ExpiresUnixMS       int64
	Status              string
	AckedBy             string
	AckedUnixMS         int64
	AckNote             string
}

type GuidanceReceipt struct {
	ID          int64
	GuidanceID  string
	AckBy       string
	AckRole     string
	AckSource   string
	AckNote     string
	AckedUnixMS int64
}

type GuidanceListFilter struct {
	TeamRunID string
	SinceTS   int64
	Limit     int
	Offset    int
	Statuses  []string
}

func (g GuidanceEvent) Payload() map[string]any {
	return decodeMeta(g.PayloadJSON)
}

func (g GuidanceEvent) TargetRoles() []string {
	return decodeStringSlice(g.TargetRolesJSON)
}

func (g GuidanceEvent) TargetMemberIDs() []string {
	return decodeStringSlice(g.TargetMemberIDsJSON)
}

func (g GuidanceEvent) TargetAgentIDs() []string {
	return decodeStringSlice(g.TargetAgentIDsJSON)
}

func (d *DB) CreateGuidanceEvent(
	ctx context.Context,
	guidanceID,
	teamID,
	teamRunID,
	kind,
	priority,
	message,
	createdBy,
	createdSub string,
	createdUnixMS,
	expiresUnixMS int64,
	payload map[string]any,
	targetRoles,
	targetMemberIDs,
	targetAgentIDs []string,
	targetOrchestratorID string,
) (*GuidanceEvent, error) {
	if d == nil || d.Pool == nil {
		return nil, errors.New("db not open")
	}
	guidanceID = strings.TrimSpace(guidanceID)
	teamID = strings.TrimSpace(teamID)
	teamRunID = strings.TrimSpace(teamRunID)
	kind = strings.TrimSpace(kind)
	priority = strings.TrimSpace(priority)
	message = strings.TrimSpace(message)
	createdBy = strings.TrimSpace(createdBy)
	createdSub = strings.TrimSpace(createdSub)
	targetOrchestratorID = strings.TrimSpace(targetOrchestratorID)
	if guidanceID == "" || teamID == "" {
		return nil, errors.New("missing guidance_id or team_id")
	}
	if kind == "" {
		return nil, errors.New("missing kind")
	}
	if priority == "" {
		return nil, errors.New("missing priority")
	}
	if message == "" {
		return nil, errors.New("missing message")
	}
	if createdUnixMS <= 0 {
		createdUnixMS = time.Now().UnixMilli()
	}
	if expiresUnixMS < 0 {
		expiresUnixMS = 0
	}
	payloadJSON, err := json.Marshal(coalesceMeta(payload))
	if err != nil {
		return nil, err
	}
	targetRolesJSON, err := json.Marshal(coalesceStringSlice(targetRoles))
	if err != nil {
		return nil, err
	}
	targetMemberIDsJSON, err := json.Marshal(coalesceStringSlice(targetMemberIDs))
	if err != nil {
		return nil, err
	}
	targetAgentIDsJSON, err := json.Marshal(coalesceStringSlice(targetAgentIDs))
	if err != nil {
		return nil, err
	}
	_, err = d.Pool.Exec(ctx, `
		INSERT INTO broker_guidance_events(
			guidance_id, team_id, team_run_id, kind, priority, message,
			payload, target_roles, target_member_ids, target_agent_ids,
			target_orchestrator_id, created_by, created_sub, created_unix_ms,
			expires_unix_ms, status, acked_by, acked_unix_ms, ack_note
		)
		VALUES(
			$1, $2, $3, $4, $5, $6,
			$7::jsonb, $8::jsonb, $9::jsonb, $10::jsonb,
			$11, $12, $13, $14,
			$15, $16, $17, $18, $19
		)
	`, guidanceID, teamID, nullIfEmpty(teamRunID), kind, priority, message,
		string(payloadJSON), string(targetRolesJSON), string(targetMemberIDsJSON), string(targetAgentIDsJSON),
		nullIfEmpty(targetOrchestratorID), nullIfEmpty(createdBy), nullIfEmpty(createdSub), createdUnixMS,
		expiresUnixMS, "open", nil, int64(0), nil,
	)
	if err != nil {
		return nil, err
	}
	return d.GetGuidanceEvent(ctx, teamID, guidanceID)
}

func (d *DB) GetGuidanceEvent(ctx context.Context, teamID, guidanceID string) (*GuidanceEvent, error) {
	if d == nil || d.Pool == nil {
		return nil, errors.New("db not open")
	}
	teamID = strings.TrimSpace(teamID)
	guidanceID = strings.TrimSpace(guidanceID)
	if teamID == "" || guidanceID == "" {
		return nil, errors.New("missing team_id or guidance_id")
	}
	var out GuidanceEvent
	var teamRunID *string
	var targetOrchID *string
	var createdBy *string
	var createdSub *string
	var ackedBy *string
	var ackNote *string
	err := d.Pool.QueryRow(ctx, `
		SELECT guidance_id, team_id, team_run_id, kind, priority, message,
		       payload::text, target_roles::text, target_member_ids::text, target_agent_ids::text,
		       target_orchestrator_id, created_by, created_sub, created_unix_ms,
		       expires_unix_ms, status, acked_by, acked_unix_ms, ack_note
		FROM broker_guidance_events
		WHERE team_id=$1 AND guidance_id=$2
	`, teamID, guidanceID).Scan(
		&out.GuidanceID,
		&out.TeamID,
		&teamRunID,
		&out.Kind,
		&out.Priority,
		&out.Message,
		&out.PayloadJSON,
		&out.TargetRolesJSON,
		&out.TargetMemberIDsJSON,
		&out.TargetAgentIDsJSON,
		&targetOrchID,
		&createdBy,
		&createdSub,
		&out.CreatedUnixMS,
		&out.ExpiresUnixMS,
		&out.Status,
		&ackedBy,
		&out.AckedUnixMS,
		&ackNote,
	)
	if err != nil {
		return nil, err
	}
	if teamRunID != nil {
		out.TeamRunID = *teamRunID
	}
	if targetOrchID != nil {
		out.TargetOrchestrator = *targetOrchID
	}
	if createdBy != nil {
		out.CreatedBy = *createdBy
	}
	if createdSub != nil {
		out.CreatedSub = *createdSub
	}
	if ackedBy != nil {
		out.AckedBy = *ackedBy
	}
	if ackNote != nil {
		out.AckNote = *ackNote
	}
	return &out, nil
}

func (d *DB) ListGuidanceEvents(ctx context.Context, teamID string, filter GuidanceListFilter) ([]GuidanceEvent, error) {
	if d == nil || d.Pool == nil {
		return nil, errors.New("db not open")
	}
	teamID = strings.TrimSpace(teamID)
	filter.TeamRunID = strings.TrimSpace(filter.TeamRunID)
	if teamID == "" {
		return nil, errors.New("missing team_id")
	}
	if filter.Limit <= 0 {
		filter.Limit = 200
	}
	if filter.Limit > 500 {
		filter.Limit = 500
	}
	if filter.Offset < 0 {
		filter.Offset = 0
	}
	if filter.SinceTS < 0 {
		filter.SinceTS = 0
	}
	statuses := make([]string, 0, len(filter.Statuses))
	for _, s := range filter.Statuses {
		s = strings.TrimSpace(s)
		if s != "" {
			statuses = append(statuses, s)
		}
	}
	var rows pgx.Rows
	var err error
	switch {
	case filter.TeamRunID != "" && len(statuses) > 0:
		rows, err = d.Pool.Query(ctx, `
			SELECT guidance_id, team_id, team_run_id, kind, priority, message,
			       payload::text, target_roles::text, target_member_ids::text, target_agent_ids::text,
			       target_orchestrator_id, created_by, created_sub, created_unix_ms,
			       expires_unix_ms, status, acked_by, acked_unix_ms, ack_note
			FROM broker_guidance_events
			WHERE team_id=$1 AND team_run_id=$2 AND created_unix_ms >= $3 AND status = ANY($4)
			ORDER BY created_unix_ms DESC, guidance_id ASC
			LIMIT $5 OFFSET $6
		`, teamID, filter.TeamRunID, filter.SinceTS, statuses, filter.Limit, filter.Offset)
	case filter.TeamRunID != "":
		rows, err = d.Pool.Query(ctx, `
			SELECT guidance_id, team_id, team_run_id, kind, priority, message,
			       payload::text, target_roles::text, target_member_ids::text, target_agent_ids::text,
			       target_orchestrator_id, created_by, created_sub, created_unix_ms,
			       expires_unix_ms, status, acked_by, acked_unix_ms, ack_note
			FROM broker_guidance_events
			WHERE team_id=$1 AND team_run_id=$2 AND created_unix_ms >= $3
			ORDER BY created_unix_ms DESC, guidance_id ASC
			LIMIT $4 OFFSET $5
		`, teamID, filter.TeamRunID, filter.SinceTS, filter.Limit, filter.Offset)
	case len(statuses) > 0:
		rows, err = d.Pool.Query(ctx, `
			SELECT guidance_id, team_id, team_run_id, kind, priority, message,
			       payload::text, target_roles::text, target_member_ids::text, target_agent_ids::text,
			       target_orchestrator_id, created_by, created_sub, created_unix_ms,
			       expires_unix_ms, status, acked_by, acked_unix_ms, ack_note
			FROM broker_guidance_events
			WHERE team_id=$1 AND created_unix_ms >= $2 AND status = ANY($3)
			ORDER BY created_unix_ms DESC, guidance_id ASC
			LIMIT $4 OFFSET $5
		`, teamID, filter.SinceTS, statuses, filter.Limit, filter.Offset)
	default:
		rows, err = d.Pool.Query(ctx, `
			SELECT guidance_id, team_id, team_run_id, kind, priority, message,
			       payload::text, target_roles::text, target_member_ids::text, target_agent_ids::text,
			       target_orchestrator_id, created_by, created_sub, created_unix_ms,
			       expires_unix_ms, status, acked_by, acked_unix_ms, ack_note
			FROM broker_guidance_events
			WHERE team_id=$1 AND created_unix_ms >= $2
			ORDER BY created_unix_ms DESC, guidance_id ASC
			LIMIT $3 OFFSET $4
		`, teamID, filter.SinceTS, filter.Limit, filter.Offset)
	}
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	out := []GuidanceEvent{}
	for rows.Next() {
		var row GuidanceEvent
		var teamRunID *string
		var targetOrchID *string
		var createdBy *string
		var createdSub *string
		var ackedBy *string
		var ackNote *string
		if err := rows.Scan(
			&row.GuidanceID,
			&row.TeamID,
			&teamRunID,
			&row.Kind,
			&row.Priority,
			&row.Message,
			&row.PayloadJSON,
			&row.TargetRolesJSON,
			&row.TargetMemberIDsJSON,
			&row.TargetAgentIDsJSON,
			&targetOrchID,
			&createdBy,
			&createdSub,
			&row.CreatedUnixMS,
			&row.ExpiresUnixMS,
			&row.Status,
			&ackedBy,
			&row.AckedUnixMS,
			&ackNote,
		); err != nil {
			return nil, err
		}
		if teamRunID != nil {
			row.TeamRunID = *teamRunID
		}
		if targetOrchID != nil {
			row.TargetOrchestrator = *targetOrchID
		}
		if createdBy != nil {
			row.CreatedBy = *createdBy
		}
		if createdSub != nil {
			row.CreatedSub = *createdSub
		}
		if ackedBy != nil {
			row.AckedBy = *ackedBy
		}
		if ackNote != nil {
			row.AckNote = *ackNote
		}
		out = append(out, row)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	return out, nil
}

func (d *DB) UpdateGuidanceAck(
	ctx context.Context,
	teamID,
	guidanceID,
	status,
	ackedBy,
	ackNote string,
	ackedUnixMS int64,
) (*GuidanceEvent, error) {
	if d == nil || d.Pool == nil {
		return nil, errors.New("db not open")
	}
	teamID = strings.TrimSpace(teamID)
	guidanceID = strings.TrimSpace(guidanceID)
	status = strings.TrimSpace(status)
	ackedBy = strings.TrimSpace(ackedBy)
	ackNote = strings.TrimSpace(ackNote)
	if teamID == "" || guidanceID == "" {
		return nil, errors.New("missing team_id or guidance_id")
	}
	if status == "" {
		return nil, errors.New("missing status")
	}
	if ackedUnixMS <= 0 {
		ackedUnixMS = time.Now().UnixMilli()
	}
	tag, err := d.Pool.Exec(ctx, `
		UPDATE broker_guidance_events
		SET status=$3, acked_by=$4, acked_unix_ms=$5, ack_note=$6
		WHERE team_id=$1 AND guidance_id=$2 AND status='open'
	`, teamID, guidanceID, status, nullIfEmpty(ackedBy), ackedUnixMS, nullIfEmpty(ackNote))
	if err != nil {
		return nil, err
	}
	if tag.RowsAffected() == 0 {
		return d.GetGuidanceEvent(ctx, teamID, guidanceID)
	}
	return d.GetGuidanceEvent(ctx, teamID, guidanceID)
}

func (d *DB) CreateGuidanceReceipt(
	ctx context.Context,
	guidanceID,
	ackBy,
	ackRole,
	ackSource,
	ackNote string,
	ackedUnixMS int64,
) (*GuidanceReceipt, error) {
	if d == nil || d.Pool == nil {
		return nil, errors.New("db not open")
	}
	guidanceID = strings.TrimSpace(guidanceID)
	ackBy = strings.TrimSpace(ackBy)
	ackRole = strings.TrimSpace(ackRole)
	ackSource = strings.TrimSpace(ackSource)
	ackNote = strings.TrimSpace(ackNote)
	if guidanceID == "" || ackBy == "" {
		return nil, errors.New("missing guidance_id or ack_by")
	}
	if ackSource == "" {
		return nil, errors.New("missing ack_source")
	}
	if ackedUnixMS <= 0 {
		ackedUnixMS = time.Now().UnixMilli()
	}
	var out GuidanceReceipt
	err := d.Pool.QueryRow(ctx, `
		INSERT INTO broker_guidance_receipts(
			guidance_id, ack_by, ack_role, ack_source, ack_note, acked_unix_ms
		)
		VALUES($1, $2, $3, $4, $5, $6)
		RETURNING id, guidance_id, ack_by, COALESCE(ack_role, ''), ack_source, COALESCE(ack_note, ''), acked_unix_ms
	`, guidanceID, ackBy, nullIfEmpty(ackRole), ackSource, nullIfEmpty(ackNote), ackedUnixMS).Scan(
		&out.ID,
		&out.GuidanceID,
		&out.AckBy,
		&out.AckRole,
		&out.AckSource,
		&out.AckNote,
		&out.AckedUnixMS,
	)
	if err != nil {
		return nil, err
	}
	return &out, nil
}
