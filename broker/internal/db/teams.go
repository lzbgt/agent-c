package db

import (
	"context"
	"encoding/json"
	"errors"
	"strings"
	"time"
)

type Team struct {
	TeamID              string
	OwnerSub            string
	DisplayName         string
	TagsJSON            json.RawMessage
	PolicyRef           string
	SharedMemoryScopeID string
	MetaJSON            json.RawMessage
	CreatedAt           time.Time
}

func (t Team) Tags() []string {
	return decodeStringSlice(t.TagsJSON)
}

func (t Team) Meta() map[string]any {
	return decodeMeta(t.MetaJSON)
}

type TeamMember struct {
	MemberID         string
	TeamID           string
	DeploymentID     string
	AgentID          string
	Role             string
	CapabilitiesJSON json.RawMessage
	Status           string
	Weight           int
	MetaJSON         json.RawMessage
	CreatedAt        time.Time
}

func (m TeamMember) Capabilities() []string {
	return decodeStringSlice(m.CapabilitiesJSON)
}

func (m TeamMember) Meta() map[string]any {
	return decodeMeta(m.MetaJSON)
}

type TeamQuorumRule struct {
	RuleID               string
	TeamID               string
	Action               string
	ToolNamesJSON        json.RawMessage
	MinApprovals         int
	RoleAllowlistJSON    json.RawMessage
	RequireDistinctRoles bool
	TimeoutMS            int64
	QuorumMode           string
	MetaJSON             json.RawMessage
	CreatedAt            time.Time
}

func (r TeamQuorumRule) ToolNames() []string {
	return decodeStringSlice(r.ToolNamesJSON)
}

func (r TeamQuorumRule) RoleAllowlist() []string {
	return decodeStringSlice(r.RoleAllowlistJSON)
}

func (r TeamQuorumRule) Meta() map[string]any {
	return decodeMeta(r.MetaJSON)
}

type TeamRun struct {
	TeamRunID string
	TeamID    string
	Status    string
	CreatedBy string
	RunJSON   json.RawMessage
	CreatedAt time.Time
}

func (d *DB) CreateTeam(ctx context.Context, ownerSub, teamID, displayName string, tags []string, policyRef, sharedMemoryScopeID string, meta map[string]any) (*Team, error) {
	if d == nil || d.Pool == nil {
		return nil, errors.New("db not open")
	}
	ownerSub = strings.TrimSpace(ownerSub)
	teamID = strings.TrimSpace(teamID)
	displayName = strings.TrimSpace(displayName)
	if ownerSub == "" {
		return nil, errors.New("missing owner sub")
	}
	if teamID == "" {
		return nil, errors.New("missing team_id")
	}
	if displayName == "" {
		return nil, errors.New("missing display_name")
	}
	if err := d.EnsureUser(ctx, ownerSub); err != nil {
		return nil, err
	}
	tagsJSON, _ := json.Marshal(coalesceStringSlice(tags))
	metaJSON, _ := json.Marshal(coalesceMap(meta))
	_, err := d.Pool.Exec(ctx, `
		INSERT INTO broker_teams(team_id, owner_sub, display_name, tags, policy_ref, shared_memory_scope_id, meta)
		VALUES($1, $2, $3, $4::jsonb, $5, $6, $7::jsonb)
	`, teamID, ownerSub, displayName, string(tagsJSON), nullIfEmpty(policyRef), nullIfEmpty(sharedMemoryScopeID), string(metaJSON))
	if err != nil {
		return nil, err
	}
	return d.GetTeam(ctx, teamID)
}

func (d *DB) ListTeamsForUser(ctx context.Context, ownerSub string) ([]Team, error) {
	if d == nil || d.Pool == nil {
		return nil, errors.New("db not open")
	}
	ownerSub = strings.TrimSpace(ownerSub)
	if ownerSub == "" {
		return nil, errors.New("missing owner sub")
	}
	if err := d.EnsureUser(ctx, ownerSub); err != nil {
		return nil, err
	}
	rows, err := d.Pool.Query(ctx, `
		SELECT team_id, owner_sub, display_name, tags::text, policy_ref, shared_memory_scope_id, meta::text, created_at
		FROM broker_teams
		WHERE owner_sub=$1
		ORDER BY created_at DESC, team_id ASC
	`, ownerSub)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Team
	for rows.Next() {
		var t Team
		var tags, meta string
		if err := rows.Scan(&t.TeamID, &t.OwnerSub, &t.DisplayName, &tags, &t.PolicyRef, &t.SharedMemoryScopeID, &meta, &t.CreatedAt); err != nil {
			return nil, err
		}
		t.TagsJSON = json.RawMessage(tags)
		t.MetaJSON = json.RawMessage(meta)
		out = append(out, t)
	}
	return out, rows.Err()
}

func (d *DB) ListTeamsAll(ctx context.Context) ([]Team, error) {
	if d == nil || d.Pool == nil {
		return nil, errors.New("db not open")
	}
	rows, err := d.Pool.Query(ctx, `
		SELECT team_id, owner_sub, display_name, tags::text, policy_ref, shared_memory_scope_id, meta::text, created_at
		FROM broker_teams
		ORDER BY created_at DESC, team_id ASC
	`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Team
	for rows.Next() {
		var t Team
		var tags, meta string
		if err := rows.Scan(&t.TeamID, &t.OwnerSub, &t.DisplayName, &tags, &t.PolicyRef, &t.SharedMemoryScopeID, &meta, &t.CreatedAt); err != nil {
			return nil, err
		}
		t.TagsJSON = json.RawMessage(tags)
		t.MetaJSON = json.RawMessage(meta)
		out = append(out, t)
	}
	return out, rows.Err()
}

func (d *DB) GetTeam(ctx context.Context, teamID string) (*Team, error) {
	if d == nil || d.Pool == nil {
		return nil, errors.New("db not open")
	}
	teamID = strings.TrimSpace(teamID)
	if teamID == "" {
		return nil, errors.New("missing team_id")
	}
	var t Team
	var tags, meta string
	err := d.Pool.QueryRow(ctx, `
		SELECT team_id, owner_sub, display_name, tags::text, policy_ref, shared_memory_scope_id, meta::text, created_at
		FROM broker_teams
		WHERE team_id=$1
	`, teamID).Scan(&t.TeamID, &t.OwnerSub, &t.DisplayName, &tags, &t.PolicyRef, &t.SharedMemoryScopeID, &meta, &t.CreatedAt)
	if err != nil {
		return nil, err
	}
	t.TagsJSON = json.RawMessage(tags)
	t.MetaJSON = json.RawMessage(meta)
	return &t, nil
}

func (d *DB) UpdateTeam(ctx context.Context, teamID, displayName string, tags []string, policyRef, sharedMemoryScopeID string, meta map[string]any) (*Team, error) {
	if d == nil || d.Pool == nil {
		return nil, errors.New("db not open")
	}
	teamID = strings.TrimSpace(teamID)
	if teamID == "" {
		return nil, errors.New("missing team_id")
	}
	displayName = strings.TrimSpace(displayName)
	if displayName == "" {
		return nil, errors.New("missing display_name")
	}
	tagsJSON, _ := json.Marshal(coalesceStringSlice(tags))
	metaJSON, _ := json.Marshal(coalesceMap(meta))
	_, err := d.Pool.Exec(ctx, `
		UPDATE broker_teams
		SET display_name=$2, tags=$3::jsonb, policy_ref=$4, shared_memory_scope_id=$5, meta=$6::jsonb
		WHERE team_id=$1
	`, teamID, displayName, string(tagsJSON), nullIfEmpty(policyRef), nullIfEmpty(sharedMemoryScopeID), string(metaJSON))
	if err != nil {
		return nil, err
	}
	return d.GetTeam(ctx, teamID)
}

func (d *DB) DeleteTeam(ctx context.Context, teamID string) error {
	if d == nil || d.Pool == nil {
		return errors.New("db not open")
	}
	teamID = strings.TrimSpace(teamID)
	if teamID == "" {
		return errors.New("missing team_id")
	}
	_, err := d.Pool.Exec(ctx, `DELETE FROM broker_teams WHERE team_id=$1`, teamID)
	return err
}

func (d *DB) ListTeamMembers(ctx context.Context, teamID string) ([]TeamMember, error) {
	if d == nil || d.Pool == nil {
		return nil, errors.New("db not open")
	}
	teamID = strings.TrimSpace(teamID)
	if teamID == "" {
		return nil, errors.New("missing team_id")
	}
	rows, err := d.Pool.Query(ctx, `
		SELECT member_id, team_id, deployment_id, agent_id, role, capabilities::text, status, weight, meta::text, created_at
		FROM broker_team_members
		WHERE team_id=$1
		ORDER BY created_at ASC, member_id ASC
	`, teamID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []TeamMember
	for rows.Next() {
		var m TeamMember
		var caps, meta string
		if err := rows.Scan(&m.MemberID, &m.TeamID, &m.DeploymentID, &m.AgentID, &m.Role, &caps, &m.Status, &m.Weight, &meta, &m.CreatedAt); err != nil {
			return nil, err
		}
		m.CapabilitiesJSON = json.RawMessage(caps)
		m.MetaJSON = json.RawMessage(meta)
		out = append(out, m)
	}
	return out, rows.Err()
}

func (d *DB) GetTeamMember(ctx context.Context, teamID, memberID string) (*TeamMember, error) {
	if d == nil || d.Pool == nil {
		return nil, errors.New("db not open")
	}
	teamID = strings.TrimSpace(teamID)
	memberID = strings.TrimSpace(memberID)
	if teamID == "" || memberID == "" {
		return nil, errors.New("missing team_id or member_id")
	}
	var m TeamMember
	var caps, meta string
	err := d.Pool.QueryRow(ctx, `
		SELECT member_id, team_id, deployment_id, agent_id, role, capabilities::text, status, weight, meta::text, created_at
		FROM broker_team_members
		WHERE team_id=$1 AND member_id=$2
	`, teamID, memberID).Scan(&m.MemberID, &m.TeamID, &m.DeploymentID, &m.AgentID, &m.Role, &caps, &m.Status, &m.Weight, &meta, &m.CreatedAt)
	if err != nil {
		return nil, err
	}
	m.CapabilitiesJSON = json.RawMessage(caps)
	m.MetaJSON = json.RawMessage(meta)
	return &m, nil
}

func (d *DB) CreateTeamMember(ctx context.Context, teamID, memberID, deploymentID, agentID, role, status string, capabilities []string, weight int, meta map[string]any) (*TeamMember, error) {
	if d == nil || d.Pool == nil {
		return nil, errors.New("db not open")
	}
	teamID = strings.TrimSpace(teamID)
	memberID = strings.TrimSpace(memberID)
	role = strings.TrimSpace(role)
	status = strings.TrimSpace(status)
	if teamID == "" || memberID == "" {
		return nil, errors.New("missing team_id or member_id")
	}
	if role == "" {
		return nil, errors.New("missing role")
	}
	if status == "" {
		status = "active"
	}
	capsJSON, _ := json.Marshal(coalesceStringSlice(capabilities))
	metaJSON, _ := json.Marshal(coalesceMap(meta))
	_, err := d.Pool.Exec(ctx, `
		INSERT INTO broker_team_members(member_id, team_id, deployment_id, agent_id, role, capabilities, status, weight, meta)
		VALUES($1, $2, $3, $4, $5, $6::jsonb, $7, $8, $9::jsonb)
	`, memberID, teamID, nullIfEmpty(deploymentID), nullIfEmpty(agentID), role, string(capsJSON), status, weight, string(metaJSON))
	if err != nil {
		return nil, err
	}
	return d.GetTeamMember(ctx, teamID, memberID)
}

func (d *DB) UpdateTeamMember(ctx context.Context, teamID, memberID, role, status string, capabilities []string, weight int, meta map[string]any) (*TeamMember, error) {
	if d == nil || d.Pool == nil {
		return nil, errors.New("db not open")
	}
	teamID = strings.TrimSpace(teamID)
	memberID = strings.TrimSpace(memberID)
	if teamID == "" || memberID == "" {
		return nil, errors.New("missing team_id or member_id")
	}
	role = strings.TrimSpace(role)
	status = strings.TrimSpace(status)
	if role == "" {
		return nil, errors.New("missing role")
	}
	if status == "" {
		status = "active"
	}
	capsJSON, _ := json.Marshal(coalesceStringSlice(capabilities))
	metaJSON, _ := json.Marshal(coalesceMap(meta))
	_, err := d.Pool.Exec(ctx, `
		UPDATE broker_team_members
		SET role=$3, capabilities=$4::jsonb, status=$5, weight=$6, meta=$7::jsonb
		WHERE team_id=$1 AND member_id=$2
	`, teamID, memberID, role, string(capsJSON), status, weight, string(metaJSON))
	if err != nil {
		return nil, err
	}
	return d.GetTeamMember(ctx, teamID, memberID)
}

func (d *DB) DeleteTeamMember(ctx context.Context, teamID, memberID string) (bool, error) {
	if d == nil || d.Pool == nil {
		return false, errors.New("db not open")
	}
	teamID = strings.TrimSpace(teamID)
	memberID = strings.TrimSpace(memberID)
	if teamID == "" || memberID == "" {
		return false, errors.New("missing team_id or member_id")
	}
	tag, err := d.Pool.Exec(ctx, `DELETE FROM broker_team_members WHERE team_id=$1 AND member_id=$2`, teamID, memberID)
	if err != nil {
		return false, err
	}
	return tag.RowsAffected() > 0, nil
}

func (d *DB) ListTeamQuorumRules(ctx context.Context, teamID string) ([]TeamQuorumRule, error) {
	if d == nil || d.Pool == nil {
		return nil, errors.New("db not open")
	}
	teamID = strings.TrimSpace(teamID)
	if teamID == "" {
		return nil, errors.New("missing team_id")
	}
	rows, err := d.Pool.Query(ctx, `
		SELECT rule_id, team_id, action, tool_names::text, min_approvals, role_allowlist::text,
		       require_distinct_roles, timeout_ms, quorum_mode, meta::text, created_at
		FROM broker_team_quorum_rules
		WHERE team_id=$1
		ORDER BY created_at ASC, rule_id ASC
	`, teamID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []TeamQuorumRule
	for rows.Next() {
		var r TeamQuorumRule
		var tools, roles, meta string
		if err := rows.Scan(&r.RuleID, &r.TeamID, &r.Action, &tools, &r.MinApprovals, &roles, &r.RequireDistinctRoles, &r.TimeoutMS, &r.QuorumMode, &meta, &r.CreatedAt); err != nil {
			return nil, err
		}
		r.ToolNamesJSON = json.RawMessage(tools)
		r.RoleAllowlistJSON = json.RawMessage(roles)
		r.MetaJSON = json.RawMessage(meta)
		out = append(out, r)
	}
	return out, rows.Err()
}

func (d *DB) GetTeamQuorumRule(ctx context.Context, teamID, ruleID string) (*TeamQuorumRule, error) {
	if d == nil || d.Pool == nil {
		return nil, errors.New("db not open")
	}
	teamID = strings.TrimSpace(teamID)
	ruleID = strings.TrimSpace(ruleID)
	if teamID == "" || ruleID == "" {
		return nil, errors.New("missing team_id or rule_id")
	}
	var r TeamQuorumRule
	var tools, roles, meta string
	err := d.Pool.QueryRow(ctx, `
		SELECT rule_id, team_id, action, tool_names::text, min_approvals, role_allowlist::text,
		       require_distinct_roles, timeout_ms, quorum_mode, meta::text, created_at
		FROM broker_team_quorum_rules
		WHERE team_id=$1 AND rule_id=$2
	`, teamID, ruleID).Scan(&r.RuleID, &r.TeamID, &r.Action, &tools, &r.MinApprovals, &roles, &r.RequireDistinctRoles, &r.TimeoutMS, &r.QuorumMode, &meta, &r.CreatedAt)
	if err != nil {
		return nil, err
	}
	r.ToolNamesJSON = json.RawMessage(tools)
	r.RoleAllowlistJSON = json.RawMessage(roles)
	r.MetaJSON = json.RawMessage(meta)
	return &r, nil
}

func (d *DB) CreateTeamQuorumRule(ctx context.Context, teamID, ruleID, action string, toolNames []string, minApprovals int, roleAllowlist []string, requireDistinctRoles bool, timeoutMS int64, quorumMode string, meta map[string]any) (*TeamQuorumRule, error) {
	if d == nil || d.Pool == nil {
		return nil, errors.New("db not open")
	}
	teamID = strings.TrimSpace(teamID)
	ruleID = strings.TrimSpace(ruleID)
	action = strings.TrimSpace(action)
	quorumMode = strings.TrimSpace(quorumMode)
	if teamID == "" || ruleID == "" {
		return nil, errors.New("missing team_id or rule_id")
	}
	if action == "" {
		return nil, errors.New("missing action")
	}
	if minApprovals < 1 {
		return nil, errors.New("min_approvals must be >= 1")
	}
	if quorumMode == "" {
		return nil, errors.New("missing quorum_mode")
	}
	toolsJSON, _ := json.Marshal(coalesceStringSlice(toolNames))
	rolesJSON, _ := json.Marshal(coalesceStringSlice(roleAllowlist))
	metaJSON, _ := json.Marshal(coalesceMap(meta))
	_, err := d.Pool.Exec(ctx, `
		INSERT INTO broker_team_quorum_rules(rule_id, team_id, action, tool_names, min_approvals, role_allowlist,
			require_distinct_roles, timeout_ms, quorum_mode, meta)
		VALUES($1, $2, $3, $4::jsonb, $5, $6::jsonb, $7, $8, $9, $10::jsonb)
	`, ruleID, teamID, action, string(toolsJSON), minApprovals, string(rolesJSON), requireDistinctRoles, timeoutMS, quorumMode, string(metaJSON))
	if err != nil {
		return nil, err
	}
	return d.GetTeamQuorumRule(ctx, teamID, ruleID)
}

func (d *DB) UpdateTeamQuorumRule(ctx context.Context, teamID, ruleID, action string, toolNames []string, minApprovals int, roleAllowlist []string, requireDistinctRoles bool, timeoutMS int64, quorumMode string, meta map[string]any) (*TeamQuorumRule, error) {
	if d == nil || d.Pool == nil {
		return nil, errors.New("db not open")
	}
	teamID = strings.TrimSpace(teamID)
	ruleID = strings.TrimSpace(ruleID)
	if teamID == "" || ruleID == "" {
		return nil, errors.New("missing team_id or rule_id")
	}
	action = strings.TrimSpace(action)
	quorumMode = strings.TrimSpace(quorumMode)
	if action == "" {
		return nil, errors.New("missing action")
	}
	if minApprovals < 1 {
		return nil, errors.New("min_approvals must be >= 1")
	}
	if quorumMode == "" {
		return nil, errors.New("missing quorum_mode")
	}
	toolsJSON, _ := json.Marshal(coalesceStringSlice(toolNames))
	rolesJSON, _ := json.Marshal(coalesceStringSlice(roleAllowlist))
	metaJSON, _ := json.Marshal(coalesceMap(meta))
	_, err := d.Pool.Exec(ctx, `
		UPDATE broker_team_quorum_rules
		SET action=$3, tool_names=$4::jsonb, min_approvals=$5, role_allowlist=$6::jsonb,
		    require_distinct_roles=$7, timeout_ms=$8, quorum_mode=$9, meta=$10::jsonb
		WHERE team_id=$1 AND rule_id=$2
	`, teamID, ruleID, action, string(toolsJSON), minApprovals, string(rolesJSON), requireDistinctRoles, timeoutMS, quorumMode, string(metaJSON))
	if err != nil {
		return nil, err
	}
	return d.GetTeamQuorumRule(ctx, teamID, ruleID)
}

func (d *DB) DeleteTeamQuorumRule(ctx context.Context, teamID, ruleID string) (bool, error) {
	if d == nil || d.Pool == nil {
		return false, errors.New("db not open")
	}
	teamID = strings.TrimSpace(teamID)
	ruleID = strings.TrimSpace(ruleID)
	if teamID == "" || ruleID == "" {
		return false, errors.New("missing team_id or rule_id")
	}
	tag, err := d.Pool.Exec(ctx, `DELETE FROM broker_team_quorum_rules WHERE team_id=$1 AND rule_id=$2`, teamID, ruleID)
	if err != nil {
		return false, err
	}
	return tag.RowsAffected() > 0, nil
}

func decodeStringSlice(raw json.RawMessage) []string {
	if len(raw) == 0 {
		return []string{}
	}
	var out []string
	_ = json.Unmarshal(raw, &out)
	if out == nil {
		out = []string{}
	}
	return out
}

func decodeMeta(raw json.RawMessage) map[string]any {
	if len(raw) == 0 {
		return map[string]any{}
	}
	var out map[string]any
	_ = json.Unmarshal(raw, &out)
	if out == nil {
		out = map[string]any{}
	}
	return out
}

func coalesceStringSlice(v []string) []string {
	if v == nil {
		return []string{}
	}
	return v
}

func nullIfEmpty(v string) any {
	if strings.TrimSpace(v) == "" {
		return nil
	}
	return v
}
