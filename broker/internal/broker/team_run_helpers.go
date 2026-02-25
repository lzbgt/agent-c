package broker

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
	"time"

	"agentd-broker/internal/db"
)

type teamRunApproval struct {
	RuleID   string
	MemberID string
	Decision string
	Reason   string
}

type teamRunApprovalRecord struct {
	RuleID   string
	MemberID string
	Role     string
	Decision string
	Reason   string
}

type teamRunQuorumRuleEval struct {
	RuleID                string
	Action                string
	QuorumMode            string
	MinApprovals          int
	Approved              int
	Missing               int
	RoleAllowlist         []string
	RequireDistinctRoles  bool
	ApprovedMemberIDs     []string
	ApprovedDistinctRoles []string
}

type teamRunQuorumEval struct {
	Rules    []teamRunQuorumRuleEval
	StrictOK bool
}

func (e teamRunQuorumEval) toJSON() map[string]any {
	rules := make([]map[string]any, 0, len(e.Rules))
	for _, r := range e.Rules {
		out := map[string]any{
			"rule_id":                r.RuleID,
			"action":                 r.Action,
			"quorum_mode":            r.QuorumMode,
			"min_approvals":          r.MinApprovals,
			"approved":               r.Approved,
			"missing":                r.Missing,
			"role_allowlist":         r.RoleAllowlist,
			"require_distinct_roles": r.RequireDistinctRoles,
			"ok":                     r.Missing == 0,
		}
		if len(r.ApprovedMemberIDs) > 0 {
			out["approved_member_ids"] = r.ApprovedMemberIDs
		}
		if len(r.ApprovedDistinctRoles) > 0 {
			out["approved_roles"] = r.ApprovedDistinctRoles
		}
		rules = append(rules, out)
	}
	return map[string]any{
		"strict_ok": e.StrictOK,
		"rules":     rules,
	}
}

type teamRunOptions struct {
	Mode           string
	Role           string
	Roles          map[string]bool
	MaxConcurrency int
	TimeoutMS      int
}

type teamRunOverrides struct {
	Mode            string
	MemberOverrides map[string]map[string]any
}

type teamRuntimeMemberInput struct {
	MemberID     string
	AgentID      string
	DeploymentID string
	Role         string
	Status       string
	Weight       int
	Capabilities []string
	Meta         map[string]any
}

func parseTeamRunOptions(meta map[string]any) (teamRunOptions, error) {
	out := teamRunOptions{
		Mode:           "sync",
		MaxConcurrency: 4,
		TimeoutMS:      60_000,
	}
	if v, ok := meta["mode"]; ok {
		s, ok := v.(string)
		if !ok {
			return out, fmt.Errorf("mode must be string")
		}
		mode := strings.ToLower(strings.TrimSpace(s))
		if mode == "" {
			mode = "sync"
		}
		if mode != "sync" && mode != "async" {
			return out, fmt.Errorf("invalid mode (sync|async)")
		}
		out.Mode = mode
	}
	if v, ok := meta["max_concurrency"]; ok {
		if iv, ok := asInt(v); ok {
			if iv < 1 {
				iv = 1
			}
			if iv > 16 {
				iv = 16
			}
			out.MaxConcurrency = iv
		}
	}
	if v, ok := meta["timeout_ms"]; ok {
		if iv, ok := asInt(v); ok {
			if iv < 100 {
				iv = 100
			}
			if iv > 300_000 {
				iv = 300_000
			}
			out.TimeoutMS = iv
		}
	}
	if v, ok := meta["role"]; ok {
		if s, ok := v.(string); ok {
			out.Role = strings.ToLower(strings.TrimSpace(s))
		}
	}
	if v, ok := meta["roles"]; ok {
		if roles := asStringSlice(v); len(roles) > 0 {
			out.Roles = map[string]bool{}
			for _, r := range roles {
				r = strings.ToLower(strings.TrimSpace(r))
				if r != "" {
					out.Roles[r] = true
				}
			}
		}
	}
	return out, nil
}

func parseTeamRunOverrides(meta map[string]any) (teamRunOverrides, error) {
	out := teamRunOverrides{Mode: "off"}
	if v, ok := meta["run_overrides_mode"]; ok {
		s, ok := v.(string)
		if !ok {
			return out, fmt.Errorf("run_overrides_mode must be string")
		}
		mode := strings.ToLower(strings.TrimSpace(s))
		if mode == "" {
			mode = "off"
		}
		switch mode {
		case "off", "member_meta", "explicit":
			out.Mode = mode
		default:
			return out, fmt.Errorf("invalid run_overrides_mode")
		}
	}
	if out.Mode == "explicit" {
		if v, ok := meta["member_overrides"]; ok {
			overrides, err := parseMemberOverrides(v)
			if err != nil {
				return out, err
			}
			out.MemberOverrides = overrides
		}
	}
	return out, nil
}

func parseTeamRunRuntimeMembers(meta map[string]any) ([]teamRuntimeMemberInput, error) {
	raw, ok := meta["runtime_members"]
	if !ok || raw == nil {
		return nil, nil
	}
	items, ok := raw.([]any)
	if !ok {
		return nil, fmt.Errorf("runtime_members must be array")
	}
	out := make([]teamRuntimeMemberInput, 0, len(items))
	for idx, item := range items {
		m, ok := item.(map[string]any)
		if !ok {
			return nil, fmt.Errorf("runtime_members[%d] must be object", idx)
		}
		memberID, _ := m["member_id"].(string)
		agentID, _ := m["agent_id"].(string)
		deploymentID, _ := m["deployment_id"].(string)
		role, _ := m["role"].(string)
		status, _ := m["status"].(string)
		weight, _ := asInt(m["weight"])
		caps, err := parseCapabilitiesValue(m["capabilities"])
		if err != nil {
			return nil, fmt.Errorf("runtime_members[%d].capabilities %w", idx, err)
		}
		var metaObj map[string]any
		if rawMeta, ok := m["meta"]; ok && rawMeta != nil {
			obj, ok := rawMeta.(map[string]any)
			if !ok {
				return nil, fmt.Errorf("runtime_members[%d].meta must be object", idx)
			}
			metaObj = obj
		}
		out = append(out, teamRuntimeMemberInput{
			MemberID:     strings.TrimSpace(memberID),
			AgentID:      strings.TrimSpace(agentID),
			DeploymentID: strings.TrimSpace(deploymentID),
			Role:         strings.TrimSpace(role),
			Status:       strings.TrimSpace(status),
			Weight:       weight,
			Capabilities: caps,
			Meta:         metaObj,
		})
	}
	return out, nil
}

func mergeRuntimeMemberInputs(existing, incoming []teamRuntimeMemberInput) []teamRuntimeMemberInput {
	if len(existing) == 0 {
		return incoming
	}
	if len(incoming) == 0 {
		return existing
	}
	out := make([]teamRuntimeMemberInput, 0, len(existing)+len(incoming))
	seen := map[string]int{}
	for _, item := range existing {
		if item.MemberID != "" {
			seen[item.MemberID] = len(out)
		}
		out = append(out, item)
	}
	for _, item := range incoming {
		if item.MemberID != "" {
			if idx, ok := seen[item.MemberID]; ok {
				out[idx] = item
				continue
			}
			seen[item.MemberID] = len(out)
		}
		out = append(out, item)
	}
	return out
}

func teamRunMemberJobsFromMeta(teamMeta map[string]any) []map[string]any {
	if teamMeta == nil {
		return nil
	}
	rawJobs, ok := teamMeta["member_jobs"]
	if !ok || rawJobs == nil {
		return nil
	}
	switch t := rawJobs.(type) {
	case []map[string]any:
		return t
	case []any:
		out := make([]map[string]any, 0, len(t))
		for _, item := range t {
			if m, ok := item.(map[string]any); ok {
				out = append(out, m)
			}
		}
		return out
	default:
		return nil
	}
}

func teamRunDispatchErrorCount(teamMeta map[string]any) int {
	if teamMeta == nil {
		return 0
	}
	rawErrs, ok := teamMeta["dispatch_errors"]
	if !ok || rawErrs == nil {
		return 0
	}
	switch t := rawErrs.(type) {
	case []map[string]any:
		return len(t)
	case []any:
		return len(t)
	default:
		return 0
	}
}

func teamRunMemberJobSummary(teamMeta map[string]any) map[string]any {
	items := teamRunMemberJobsFromMeta(teamMeta)
	if len(items) == 0 {
		return nil
	}
	dispatchCount := teamRunDispatchErrorCount(teamMeta)
	queued := 0
	running := 0
	done := 0
	errCount := 0
	cancelled := 0
	interrupted := 0
	unknown := 0
	okCount := 0
	failedCount := 0

	for _, item := range items {
		status := ""
		if raw, ok := item["status"].(string); ok {
			status = strings.ToLower(strings.TrimSpace(raw))
		}
		switch status {
		case "queued":
			queued++
		case "running":
			running++
		case "done":
			done++
		case "error":
			errCount++
		case "cancelled":
			cancelled++
		case "interrupted":
			interrupted++
		default:
			unknown++
		}

		jobFailed := false
		if dispatchErr, ok := item["dispatch_error"].(string); ok && strings.TrimSpace(dispatchErr) != "" {
			jobFailed = true
		}
		if errStr, ok := item["error"].(string); ok && strings.TrimSpace(errStr) != "" {
			jobFailed = true
		}
		if okVal, ok := item["ok"].(bool); ok {
			if okVal {
				okCount++
			} else {
				jobFailed = true
			}
		}
		switch status {
		case "error", "cancelled", "interrupted":
			jobFailed = true
		}
		if jobFailed {
			failedCount++
		}
	}

	summary := map[string]any{
		"total":           len(items),
		"queued":          queued,
		"running":         running,
		"done":            done,
		"error":           errCount,
		"cancelled":       cancelled,
		"interrupted":     interrupted,
		"unknown":         unknown,
		"ok":              okCount,
		"failed":          failedCount,
		"dispatch_errors": dispatchCount,
	}
	return summary
}

func parseTeamRunQuorumPolicy(meta map[string]any) (string, error) {
	mode := "auto"
	raw, ok := meta["quorum_policy"]
	if !ok || raw == nil {
		return mode, nil
	}
	switch t := raw.(type) {
	case map[string]any:
		if v, ok := t["mode"]; ok {
			if s, ok := v.(string); ok {
				mode = strings.ToLower(strings.TrimSpace(s))
			} else {
				return "", fmt.Errorf("invalid quorum_policy.mode")
			}
		}
	case string:
		mode = strings.ToLower(strings.TrimSpace(t))
	default:
		return "", fmt.Errorf("invalid quorum_policy")
	}
	if mode == "" {
		mode = "auto"
	}
	if mode != "auto" && mode != "off" {
		return "", fmt.Errorf("invalid quorum_policy.mode")
	}
	return mode, nil
}

func parseTeamRunApprovals(meta map[string]any) ([]teamRunApproval, error) {
	raw, ok := meta["approvals"]
	if !ok || raw == nil {
		return nil, nil
	}
	var items []any
	switch t := raw.(type) {
	case []any:
		items = t
	case []string:
		items = make([]any, 0, len(t))
		for _, s := range t {
			items = append(items, s)
		}
	default:
		return nil, fmt.Errorf("approvals must be an array")
	}
	out := make([]teamRunApproval, 0, len(items))
	for _, item := range items {
		switch v := item.(type) {
		case string:
			memberID := strings.TrimSpace(v)
			if memberID == "" {
				return nil, fmt.Errorf("approval member_id is required")
			}
			out = append(out, teamRunApproval{
				RuleID:   "",
				MemberID: memberID,
				Decision: "approve",
			})
		case map[string]any:
			ruleRaw, _ := v["rule_id"].(string)
			ruleID := strings.TrimSpace(ruleRaw)
			memberRaw, _ := v["member_id"].(string)
			memberID := strings.TrimSpace(memberRaw)
			if memberID == "" {
				return nil, fmt.Errorf("approval member_id is required")
			}
			decisionRaw, _ := v["decision"].(string)
			if decisionRaw == "" {
				decisionRaw = "approve"
			}
			decision, ok := normalizeApprovalDecision(decisionRaw)
			if !ok {
				return nil, fmt.Errorf("invalid approval decision: %s", decisionRaw)
			}
			reason, _ := v["reason"].(string)
			out = append(out, teamRunApproval{
				RuleID:   ruleID,
				MemberID: memberID,
				Decision: decision,
				Reason:   strings.TrimSpace(reason),
			})
		default:
			return nil, fmt.Errorf("invalid approval entry")
		}
	}
	return out, nil
}

func parseTeamRunApprovalsRequest(body []byte) ([]teamRunApproval, error) {
	if len(body) == 0 {
		return nil, fmt.Errorf("missing body")
	}
	var raw any
	if err := json.Unmarshal(body, &raw); err != nil {
		return nil, fmt.Errorf("invalid json")
	}
	switch t := raw.(type) {
	case []any:
		return parseTeamRunApprovals(map[string]any{"approvals": t})
	case map[string]any:
		if approvals, ok := t["approvals"]; ok {
			return parseTeamRunApprovals(map[string]any{"approvals": approvals})
		}
		return parseTeamRunApprovals(map[string]any{"approvals": []any{t}})
	default:
		return nil, fmt.Errorf("invalid approvals payload")
	}
}

func normalizeApprovalDecision(raw string) (string, bool) {
	switch strings.ToLower(strings.TrimSpace(raw)) {
	case "approve", "approved":
		return "approve", true
	case "deny", "denied":
		return "deny", true
	default:
		return "", false
	}
}

func isTeamRunQuorumAction(action string) bool {
	switch strings.ToLower(strings.TrimSpace(action)) {
	case "team_run", "run":
		return true
	default:
		return false
	}
}

func evaluateTeamRunQuorum(rules []db.TeamQuorumRule, approvals []teamRunApproval, membersByID map[string]db.TeamMember) (teamRunQuorumEval, error) {
	out := teamRunQuorumEval{
		StrictOK: true,
	}
	ruleIDs := map[string]bool{}
	for _, rule := range rules {
		if isTeamRunQuorumAction(rule.Action) {
			ruleIDs[rule.RuleID] = true
		}
	}
	for _, approval := range approvals {
		if approval.RuleID != "" && !ruleIDs[approval.RuleID] {
			return out, fmt.Errorf("unknown approval rule_id: %s", approval.RuleID)
		}
	}
	for _, rule := range rules {
		if !isTeamRunQuorumAction(rule.Action) {
			continue
		}
		roleAllowlist := toLowerSet(rule.RoleAllowlist())
		approvedMembers := map[string]bool{}
		approvedRoles := map[string]bool{}
		for _, approval := range approvals {
			if approval.Decision != "approve" {
				continue
			}
			if approval.RuleID != "" && approval.RuleID != rule.RuleID {
				continue
			}
			member, ok := membersByID[approval.MemberID]
			if !ok {
				return out, fmt.Errorf("unknown approval member_id: %s", approval.MemberID)
			}
			status := strings.ToLower(strings.TrimSpace(member.Status))
			if status != "" && status != "active" {
				return out, fmt.Errorf("approval member not active: %s", approval.MemberID)
			}
			role := strings.ToLower(strings.TrimSpace(member.Role))
			if len(roleAllowlist) > 0 && !roleAllowlist[role] {
				continue
			}
			if approvedMembers[approval.MemberID] {
				continue
			}
			approvedMembers[approval.MemberID] = true
			if role != "" {
				approvedRoles[role] = true
			}
		}
		approvedCount := len(approvedMembers)
		if rule.RequireDistinctRoles {
			approvedCount = len(approvedRoles)
		}
		missing := rule.MinApprovals - approvedCount
		if missing < 0 {
			missing = 0
		}
		ok := missing == 0
		if strings.ToLower(strings.TrimSpace(rule.QuorumMode)) == "strict" && !ok {
			out.StrictOK = false
		}
		ruleEval := teamRunQuorumRuleEval{
			RuleID:               rule.RuleID,
			Action:               rule.Action,
			QuorumMode:           rule.QuorumMode,
			MinApprovals:         rule.MinApprovals,
			Approved:             approvedCount,
			Missing:              missing,
			RoleAllowlist:        rule.RoleAllowlist(),
			RequireDistinctRoles: rule.RequireDistinctRoles,
		}
		if len(approvedMembers) > 0 {
			for memberID := range approvedMembers {
				ruleEval.ApprovedMemberIDs = append(ruleEval.ApprovedMemberIDs, memberID)
			}
		}
		if len(approvedRoles) > 0 {
			for role := range approvedRoles {
				ruleEval.ApprovedDistinctRoles = append(ruleEval.ApprovedDistinctRoles, role)
			}
		}
		out.Rules = append(out.Rules, ruleEval)
	}
	return out, nil
}

func filterTeamRunRules(rules []db.TeamQuorumRule) []db.TeamQuorumRule {
	out := make([]db.TeamQuorumRule, 0, len(rules))
	for _, rule := range rules {
		if isTeamRunQuorumAction(rule.Action) {
			out = append(out, rule)
		}
	}
	return out
}

func expandTeamRunApprovals(rules []db.TeamQuorumRule, approvals []teamRunApproval, membersByID map[string]db.TeamMember) ([]teamRunApprovalRecord, error) {
	ruleMap := map[string]db.TeamQuorumRule{}
	roleAllow := map[string]map[string]bool{}
	ruleIDs := make([]string, 0, len(rules))
	for _, rule := range rules {
		if !isTeamRunQuorumAction(rule.Action) {
			continue
		}
		ruleMap[rule.RuleID] = rule
		roleAllow[rule.RuleID] = toLowerSet(rule.RoleAllowlist())
		ruleIDs = append(ruleIDs, rule.RuleID)
	}
	if len(ruleMap) == 0 {
		return nil, nil
	}
	seen := map[string]bool{}
	out := make([]teamRunApprovalRecord, 0, len(approvals))
	for _, approval := range approvals {
		member, ok := membersByID[approval.MemberID]
		if !ok {
			return nil, fmt.Errorf("unknown approval member_id: %s", approval.MemberID)
		}
		status := strings.ToLower(strings.TrimSpace(member.Status))
		if status != "" && status != "active" {
			return nil, fmt.Errorf("approval member not active: %s", approval.MemberID)
		}
		role := strings.ToLower(strings.TrimSpace(member.Role))
		if role == "" {
			return nil, fmt.Errorf("approval member missing role: %s", approval.MemberID)
		}
		targetRules := ruleIDs
		if approval.RuleID != "" {
			if _, ok := ruleMap[approval.RuleID]; !ok {
				return nil, fmt.Errorf("unknown approval rule_id: %s", approval.RuleID)
			}
			targetRules = []string{approval.RuleID}
		}
		for _, ruleID := range targetRules {
			allow := roleAllow[ruleID]
			if len(allow) > 0 && !allow[role] {
				continue
			}
			key := ruleID + "\x00" + approval.MemberID
			if seen[key] {
				continue
			}
			seen[key] = true
			out = append(out, teamRunApprovalRecord{
				RuleID:   ruleID,
				MemberID: approval.MemberID,
				Role:     role,
				Decision: approval.Decision,
				Reason:   approval.Reason,
			})
		}
	}
	return out, nil
}

func (s *Server) persistTeamRunApprovals(ctx context.Context, teamID, teamRunID string, rules []db.TeamQuorumRule, approvals []teamRunApproval, membersByID map[string]db.TeamMember, createdBy string) error {
	if len(approvals) == 0 {
		return nil
	}
	records, err := expandTeamRunApprovals(rules, approvals, membersByID)
	if err != nil {
		return err
	}
	for _, rec := range records {
		approvalID := "tra_" + newID()[:12]
		if _, err := s.cfg.DB.UpsertTeamRunApproval(ctx, approvalID, teamRunID, teamID, rec.RuleID, rec.MemberID, rec.Role, rec.Decision, rec.Reason, createdBy); err != nil {
			return err
		}
	}
	return nil
}

func filterTeamRunMembers(members []db.TeamMember, role string, roles map[string]bool) []db.TeamMember {
	out := make([]db.TeamMember, 0, len(members))
	for _, m := range members {
		status := strings.ToLower(strings.TrimSpace(m.Status))
		if status != "" && status != "active" {
			continue
		}
		mRole := strings.ToLower(strings.TrimSpace(m.Role))
		if len(roles) > 0 {
			if !roles[mRole] {
				continue
			}
		} else if role != "" && mRole != role {
			continue
		}
		out = append(out, m)
	}
	return out
}

func toLowerSet(vals []string) map[string]bool {
	if len(vals) == 0 {
		return nil
	}
	out := map[string]bool{}
	for _, v := range vals {
		v = strings.ToLower(strings.TrimSpace(v))
		if v != "" {
			out[v] = true
		}
	}
	return out
}

func asInt(v any) (int, bool) {
	switch t := v.(type) {
	case int:
		return t, true
	case int64:
		return int(t), true
	case float64:
		return int(t), true
	case float32:
		return int(t), true
	default:
		return 0, false
	}
}

func asStringSlice(v any) []string {
	switch t := v.(type) {
	case []string:
		return t
	case []any:
		out := make([]string, 0, len(t))
		for _, item := range t {
			if s, ok := item.(string); ok {
				out = append(out, s)
			}
		}
		return out
	default:
		return nil
	}
}

func parseCapabilitiesValue(v any) ([]string, error) {
	if v == nil {
		return nil, nil
	}
	switch t := v.(type) {
	case string:
		parts := strings.Split(t, ",")
		out := make([]string, 0, len(parts))
		for _, part := range parts {
			part = strings.TrimSpace(part)
			if part != "" {
				out = append(out, part)
			}
		}
		return out, nil
	case []string, []any:
		raw := asStringSlice(t)
		if len(raw) == 0 {
			return nil, nil
		}
		out := make([]string, 0, len(raw))
		for _, item := range raw {
			item = strings.TrimSpace(item)
			if item != "" {
				out = append(out, item)
			}
		}
		return out, nil
	default:
		return nil, fmt.Errorf("must be array or string")
	}
}

func parseMemberOverrides(v any) (map[string]map[string]any, error) {
	if v == nil {
		return nil, nil
	}
	raw, ok := v.(map[string]any)
	if !ok {
		return nil, fmt.Errorf("member_overrides must be object")
	}
	out := map[string]map[string]any{}
	for k, rv := range raw {
		memberID := strings.TrimSpace(k)
		if memberID == "" {
			continue
		}
		m, ok := rv.(map[string]any)
		if !ok {
			return nil, fmt.Errorf("member_overrides.%s must be object", memberID)
		}
		sanitized := sanitizeRunOverrides(m)
		if len(sanitized) > 0 {
			out[memberID] = sanitized
		}
	}
	return out, nil
}

func parseRoleOverrides(v any) (map[string]map[string]any, error) {
	if v == nil {
		return nil, nil
	}
	raw, ok := v.(map[string]any)
	if !ok {
		return nil, fmt.Errorf("role_overrides must be object")
	}
	out := map[string]map[string]any{}
	for k, rv := range raw {
		role := strings.ToLower(strings.TrimSpace(k))
		if role == "" {
			continue
		}
		m, ok := rv.(map[string]any)
		if !ok {
			return nil, fmt.Errorf("role_overrides.%s must be object", role)
		}
		sanitized := sanitizeRunOverrides(m)
		if len(sanitized) > 0 {
			out[role] = sanitized
		}
	}
	return out, nil
}

func parseRoleInstructions(v any) (map[string]string, error) {
	if v == nil {
		return nil, nil
	}
	raw, ok := v.(map[string]any)
	if !ok {
		return nil, fmt.Errorf("role_instructions must be object")
	}
	out := map[string]string{}
	for k, rv := range raw {
		role := strings.ToLower(strings.TrimSpace(k))
		if role == "" {
			continue
		}
		var instr string
		switch t := rv.(type) {
		case string:
			instr = strings.TrimSpace(t)
		case map[string]any:
			if rawInstr, ok := t["instruction"]; ok {
				if s, ok := rawInstr.(string); ok {
					instr = strings.TrimSpace(s)
				}
			}
		default:
			return nil, fmt.Errorf("role_instructions.%s must be string or object", role)
		}
		if instr != "" {
			out[role] = instr
		}
	}
	return out, nil
}

func parseRolePromptMode(v any) (string, error) {
	if v == nil {
		return "", nil
	}
	raw, ok := v.(string)
	if !ok {
		return "", fmt.Errorf("role_prompt_mode must be string")
	}
	mode := strings.ToLower(strings.TrimSpace(raw))
	if mode == "" {
		return "", nil
	}
	switch mode {
	case "prepend", "append", "replace":
		return mode, nil
	default:
		return "", fmt.Errorf("role_prompt_mode must be prepend|append|replace")
	}
}

func applyRoleInstruction(instruction, goal, mode string) string {
	instr := strings.TrimSpace(instruction)
	goal = strings.TrimSpace(goal)
	if instr == "" {
		return goal
	}
	if strings.Contains(instr, "{{goal}}") {
		return strings.ReplaceAll(instr, "{{goal}}", goal)
	}
	switch mode {
	case "append":
		if goal == "" {
			return instr
		}
		return goal + "\n\n" + instr
	case "replace":
		return instr
	default:
		if goal == "" {
			return instr
		}
		return instr + "\n\n" + goal
	}
}

func normalizeTeamMetaRoleOverrides(meta map[string]any) (map[string]any, error) {
	if meta == nil {
		return meta, nil
	}
	raw, ok := meta["role_overrides"]
	if !ok {
		return meta, nil
	}
	overrides, err := parseRoleOverrides(raw)
	if err != nil {
		return meta, err
	}
	if len(overrides) > 0 {
		meta["role_overrides"] = overrides
	} else {
		delete(meta, "role_overrides")
	}
	return meta, nil
}

func buildRuntimeMembers(inputs []teamRuntimeMemberInput, teamID string, usedIDs map[string]bool) ([]db.TeamMember, []map[string]any, error) {
	if len(inputs) == 0 {
		return nil, nil, nil
	}
	if usedIDs == nil {
		usedIDs = map[string]bool{}
	}
	out := make([]db.TeamMember, 0, len(inputs))
	outJSON := make([]map[string]any, 0, len(inputs))
	for idx, input := range inputs {
		memberID := strings.TrimSpace(input.MemberID)
		if memberID == "" {
			memberID = nextRuntimeMemberID(usedIDs)
		} else if usedIDs[memberID] {
			return nil, nil, fmt.Errorf("runtime_members[%d] member_id duplicate: %s", idx, memberID)
		}
		usedIDs[memberID] = true
		agentID := strings.TrimSpace(input.AgentID)
		if agentID == "" {
			return nil, nil, fmt.Errorf("runtime_members[%d] agent_id required", idx)
		}
		role := strings.TrimSpace(input.Role)
		if role == "" {
			return nil, nil, fmt.Errorf("runtime_members[%d] role required", idx)
		}
		status := strings.TrimSpace(input.Status)
		if status == "" {
			status = "active"
		}
		meta := normalizeRuntimeMemberMeta(input.Meta)
		metaJSON, _ := json.Marshal(meta)
		caps := input.Capabilities
		if caps == nil {
			caps = []string{}
		}
		capsJSON, _ := json.Marshal(caps)
		member := db.TeamMember{
			MemberID:         memberID,
			TeamID:           teamID,
			DeploymentID:     strings.TrimSpace(input.DeploymentID),
			AgentID:          agentID,
			Role:             role,
			CapabilitiesJSON: capsJSON,
			Status:           status,
			Weight:           input.Weight,
			MetaJSON:         metaJSON,
			CreatedAt:        time.Now().UTC(),
		}
		out = append(out, member)
		outJSON = append(outJSON, teamMemberToJSON(member))
	}
	return out, outJSON, nil
}

func nextRuntimeMemberID(usedIDs map[string]bool) string {
	for {
		id := "rtm_" + newID()[:12]
		if !usedIDs[id] {
			return id
		}
	}
}

func normalizeRuntimeMemberMeta(meta map[string]any) map[string]any {
	if len(meta) == 0 {
		return meta
	}
	out := map[string]any{}
	for k, v := range meta {
		out[k] = v
	}
	if raw, ok := out["run_overrides"]; ok {
		if m, ok := raw.(map[string]any); ok {
			if sanitized := sanitizeRunOverrides(m); len(sanitized) > 0 {
				out["run_overrides"] = sanitized
			} else {
				delete(out, "run_overrides")
			}
		} else {
			delete(out, "run_overrides")
		}
	}
	return out
}

func sanitizeRunOverrides(raw map[string]any) map[string]any {
	if len(raw) == 0 {
		return nil
	}
	out := map[string]any{}
	for k, v := range raw {
		key := strings.ToLower(strings.TrimSpace(k))
		switch key {
		case "model", "base_url", "summary_model":
			if s, ok := v.(string); ok {
				s = strings.TrimSpace(s)
				if s != "" {
					out[key] = s
				}
			}
		case "tools":
			if s, ok := v.(string); ok {
				s = strings.TrimSpace(strings.ToLower(s))
				if s == "none" || s == "basic" || s == "host" {
					out[key] = s
				}
			}
		case "timeout_ms":
			if iv, ok := asInt(v); ok {
				if iv < 100 {
					iv = 100
				}
				if iv > 300_000 {
					iv = 300_000
				}
				out[key] = iv
			}
		case "max_steps":
			if iv, ok := asInt(v); ok {
				if iv < 1 {
					iv = 1
				}
				if iv > 256 {
					iv = 256
				}
				out[key] = iv
			}
		case "stream_assistant":
			if b, ok := v.(bool); ok {
				out[key] = b
			}
		}
	}
	if len(out) == 0 {
		return nil
	}
	return out
}

func memberMetaRunOverrides(meta map[string]any) map[string]any {
	if len(meta) == 0 {
		return nil
	}
	raw, ok := meta["run_overrides"]
	if !ok {
		return nil
	}
	m, ok := raw.(map[string]any)
	if !ok {
		return nil
	}
	return sanitizeRunOverrides(m)
}
