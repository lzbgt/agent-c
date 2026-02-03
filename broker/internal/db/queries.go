package db

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"time"
)

type Agent struct {
	AgentID    string
	OwnerSub   string
	Enabled    bool
	CreatedAt  time.Time
	LabelsJSON json.RawMessage
	MetaJSON   json.RawMessage
}

func (a Agent) Labels() map[string]any {
	if len(a.LabelsJSON) == 0 {
		return map[string]any{}
	}
	var out map[string]any
	_ = json.Unmarshal(a.LabelsJSON, &out)
	if out == nil {
		out = map[string]any{}
	}
	return out
}

func (a Agent) Meta() map[string]any {
	if len(a.MetaJSON) == 0 {
		return map[string]any{}
	}
	var out map[string]any
	_ = json.Unmarshal(a.MetaJSON, &out)
	if out == nil {
		out = map[string]any{}
	}
	return out
}

func (d *DB) EnsureUser(ctx context.Context, sub string) error {
	if d == nil || d.Pool == nil {
		return errors.New("db not open")
	}
	sub = strings.TrimSpace(sub)
	if sub == "" {
		return errors.New("empty sub")
	}
	_, err := d.Pool.Exec(ctx, `INSERT INTO broker_users(sub) VALUES($1) ON CONFLICT DO NOTHING`, sub)
	return err
}

func (d *DB) CreateAgent(ctx context.Context, ownerSub, agentID string, labels, meta map[string]any) (*Agent, error) {
	if d == nil || d.Pool == nil {
		return nil, errors.New("db not open")
	}
	ownerSub = strings.TrimSpace(ownerSub)
	agentID = strings.TrimSpace(agentID)
	if ownerSub == "" {
		return nil, errors.New("missing owner sub")
	}
	if agentID == "" {
		return nil, errors.New("missing agent_id")
	}
	if err := d.EnsureUser(ctx, ownerSub); err != nil {
		return nil, err
	}
	lb, _ := json.Marshal(coalesceMap(labels))
	mb, _ := json.Marshal(coalesceMap(meta))

	tx, err := d.Pool.Begin(ctx)
	if err != nil {
		return nil, err
	}
	defer func() { _ = tx.Rollback(ctx) }()

	_, err = tx.Exec(ctx, `
		INSERT INTO broker_agents(agent_id, owner_sub, enabled, labels, meta)
		VALUES($1, $2, true, $3::jsonb, $4::jsonb)
	`, agentID, ownerSub, string(lb), string(mb))
	if err != nil {
		return nil, err
	}
	_, err = tx.Exec(ctx, `
		INSERT INTO broker_agent_memberships(agent_id, user_sub, role)
		VALUES($1, $2, 'owner')
		ON CONFLICT(agent_id, user_sub) DO NOTHING
	`, agentID, ownerSub)
	if err != nil {
		return nil, err
	}

	var out Agent
	row := tx.QueryRow(ctx, `
		SELECT agent_id, owner_sub, enabled, created_at, labels, meta
		FROM broker_agents WHERE agent_id=$1
	`, agentID)
	if err := row.Scan(&out.AgentID, &out.OwnerSub, &out.Enabled, &out.CreatedAt, &out.LabelsJSON, &out.MetaJSON); err != nil {
		return nil, err
	}
	if err := tx.Commit(ctx); err != nil {
		return nil, err
	}
	return &out, nil
}

func (d *DB) ListAgentsForUser(ctx context.Context, userSub string) ([]Agent, error) {
	if d == nil || d.Pool == nil {
		return nil, errors.New("db not open")
	}
	userSub = strings.TrimSpace(userSub)
	if userSub == "" {
		return nil, errors.New("missing user sub")
	}
	if err := d.EnsureUser(ctx, userSub); err != nil {
		return nil, err
	}
	rows, err := d.Pool.Query(ctx, `
		SELECT a.agent_id, a.owner_sub, a.enabled, a.created_at, a.labels, a.meta
		FROM broker_agent_memberships m
		JOIN broker_agents a ON a.agent_id = m.agent_id
		WHERE m.user_sub = $1
		ORDER BY a.created_at DESC
	`, userSub)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	out := []Agent{}
	for rows.Next() {
		var a Agent
		if err := rows.Scan(&a.AgentID, &a.OwnerSub, &a.Enabled, &a.CreatedAt, &a.LabelsJSON, &a.MetaJSON); err != nil {
			return nil, err
		}
		out = append(out, a)
	}
	return out, rows.Err()
}

func (d *DB) UserCanAccessAgent(ctx context.Context, userSub, agentID string) (bool, error) {
	if d == nil || d.Pool == nil {
		return false, errors.New("db not open")
	}
	userSub = strings.TrimSpace(userSub)
	agentID = strings.TrimSpace(agentID)
	if userSub == "" || agentID == "" {
		return false, nil
	}
	var ok bool
	err := d.Pool.QueryRow(ctx, `
		SELECT EXISTS(
			SELECT 1
			FROM broker_agent_memberships m
			JOIN broker_agents a ON a.agent_id = m.agent_id
			WHERE m.user_sub=$1 AND m.agent_id=$2 AND a.enabled=true
		)
	`, userSub, agentID).Scan(&ok)
	return ok, err
}

func (d *DB) AgentEnabled(ctx context.Context, agentID string) (bool, error) {
	if d == nil || d.Pool == nil {
		return false, errors.New("db not open")
	}
	agentID = strings.TrimSpace(agentID)
	if agentID == "" {
		return false, errors.New("missing agent_id")
	}
	var enabled bool
	err := d.Pool.QueryRow(ctx, `SELECT enabled FROM broker_agents WHERE agent_id=$1`, agentID).Scan(&enabled)
	if err != nil {
		return false, err
	}
	return enabled, nil
}

func (d *DB) ListAgentMemberSubs(ctx context.Context, agentID string) ([]string, error) {
	if d == nil || d.Pool == nil {
		return nil, errors.New("db not open")
	}
	agentID = strings.TrimSpace(agentID)
	if agentID == "" {
		return nil, errors.New("missing agent_id")
	}
	rows, err := d.Pool.Query(ctx, `SELECT user_sub FROM broker_agent_memberships WHERE agent_id=$1`, agentID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []string{}
	for rows.Next() {
		var sub string
		if err := rows.Scan(&sub); err != nil {
			return nil, err
		}
		sub = strings.TrimSpace(sub)
		if sub != "" {
			out = append(out, sub)
		}
	}
	return out, rows.Err()
}

func (d *DB) InsertConnection(ctx context.Context, agentID, remoteAddr string, meta map[string]any) (int64, error) {
	if d == nil || d.Pool == nil {
		return 0, errors.New("db not open")
	}
	agentID = strings.TrimSpace(agentID)
	if agentID == "" {
		return 0, errors.New("missing agent_id")
	}
	mb, _ := json.Marshal(coalesceMap(meta))
	var id int64
	err := d.Pool.QueryRow(ctx, `
		INSERT INTO broker_agent_connections(agent_id, remote_addr, meta)
		VALUES($1, $2, $3::jsonb)
		RETURNING id
	`, agentID, strings.TrimSpace(remoteAddr), string(mb)).Scan(&id)
	return id, err
}

func (d *DB) MarkConnectionDisconnected(ctx context.Context, connID int64) error {
	if d == nil || d.Pool == nil {
		return errors.New("db not open")
	}
	if connID <= 0 {
		return nil
	}
	_, err := d.Pool.Exec(ctx, `
		UPDATE broker_agent_connections
		SET disconnected_at=now(), last_seen=now()
		WHERE id=$1 AND disconnected_at IS NULL
	`, connID)
	return err
}

func (d *DB) InsertRelayAudit(ctx context.Context, userSub, agentID, method, path string, status, latencyMs int, errStr, traceID string) error {
	if d == nil || d.Pool == nil {
		return errors.New("db not open")
	}
	userSub = strings.TrimSpace(userSub)
	agentID = strings.TrimSpace(agentID)
	traceID = strings.TrimSpace(traceID)
	if userSub == "" || agentID == "" {
		return errors.New("missing user_sub or agent_id")
	}
	if err := d.EnsureUser(ctx, userSub); err != nil {
		return err
	}
	if status < 0 {
		status = 0
	}
	if latencyMs < 0 {
		latencyMs = 0
	}
	_, err := d.Pool.Exec(ctx, `
		INSERT INTO broker_relay_audit(user_sub, agent_id, method, path, status, latency_ms, error, trace_id)
		VALUES($1, $2, $3, $4, $5, $6, $7, $8)
	`, userSub, agentID, strings.TrimSpace(method), strings.TrimSpace(path), status, latencyMs, strings.TrimSpace(errStr), traceID)
	return err
}

func (d *DB) DeleteAgentIfOwner(ctx context.Context, ownerSub, agentID string) error {
	if d == nil || d.Pool == nil {
		return errors.New("db not open")
	}
	ownerSub = strings.TrimSpace(ownerSub)
	agentID = strings.TrimSpace(agentID)
	if ownerSub == "" || agentID == "" {
		return errors.New("missing owner sub or agent_id")
	}
	tag, err := d.Pool.Exec(ctx, `DELETE FROM broker_agents WHERE agent_id=$1 AND owner_sub=$2`, agentID, ownerSub)
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return fmt.Errorf("not owner or agent not found")
	}
	return nil
}

func (d *DB) DeleteAgent(ctx context.Context, agentID string) error {
	if d == nil || d.Pool == nil {
		return errors.New("db not open")
	}
	agentID = strings.TrimSpace(agentID)
	if agentID == "" {
		return errors.New("missing agent_id")
	}
	_, err := d.Pool.Exec(ctx, `DELETE FROM broker_agents WHERE agent_id=$1`, agentID)
	return err
}

func coalesceMap(m map[string]any) map[string]any {
	if m == nil {
		return map[string]any{}
	}
	return m
}
