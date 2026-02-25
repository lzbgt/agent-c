package broker

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"strings"
	"time"

	"agentd-broker/internal/db"
	"agentd-broker/internal/events"
)

func (s *Server) handleTeams(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case "GET":
		s.handleTeamsList(w, r)
	case "POST":
		s.handleTeamsCreate(w, r)
	default:
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
	}
}

func (s *Server) handleTeamsList(w http.ResponseWriter, r *http.Request) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if p.AuthKind != "oidc" {
		writeErrorJSON(w, "oidc required", http.StatusForbidden)
		return
	}
	var teams []db.Team
	if p.Admin {
		teams, err = s.cfg.DB.ListTeamsAll(r.Context())
	} else {
		teams, err = s.cfg.DB.ListTeamsForUser(r.Context(), p.Sub)
	}
	if err != nil {
		writeErrorJSON(w, "db error", http.StatusInternalServerError)
		return
	}
	out := make([]map[string]any, 0, len(teams))
	for _, t := range teams {
		out = append(out, teamToJSON(t))
	}
	writeJSON(w, map[string]any{"ok": true, "teams": out})
}

func (s *Server) handleTeamsCreate(w http.ResponseWriter, r *http.Request) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if p.AuthKind != "oidc" {
		writeErrorJSON(w, "oidc required", http.StatusForbidden)
		return
	}
	body, err := readBodyBounded(r.Body, 1024*1024)
	if err != nil {
		writeErrorJSON(w, "invalid body", http.StatusBadRequest)
		return
	}
	req := struct {
		TeamID              string         `json:"team_id"`
		DisplayName         string         `json:"display_name"`
		Tags                []string       `json:"tags"`
		PolicyRef           string         `json:"policy_ref"`
		SharedMemoryScopeID string         `json:"shared_memory_scope_id"`
		Meta                map[string]any `json:"meta"`
	}{}
	if len(body) > 0 {
		if err := json.Unmarshal(body, &req); err != nil {
			writeErrorJSON(w, "invalid json", http.StatusBadRequest)
			return
		}
	}
	teamID := strings.TrimSpace(req.TeamID)
	if teamID == "" {
		teamID = "team_" + newID()[:12]
	}
	if !teamIDRe.MatchString(teamID) {
		writeErrorJSON(w, "invalid team_id", http.StatusBadRequest)
		return
	}
	displayName := strings.TrimSpace(req.DisplayName)
	if displayName == "" {
		writeErrorJSON(w, "missing display_name", http.StatusBadRequest)
		return
	}
	meta, err := normalizeTeamMetaRoleOverrides(req.Meta)
	if err != nil {
		writeErrorJSON(w, "invalid role_overrides", http.StatusBadRequest)
		return
	}
	t, err := s.cfg.DB.CreateTeam(r.Context(), p.Sub, teamID, displayName, req.Tags, req.PolicyRef, req.SharedMemoryScopeID, meta)
	if err != nil {
		writeErrorJSON(w, "create team failed", http.StatusBadRequest)
		return
	}
	writeJSON(w, map[string]any{"ok": true, "team": teamToJSON(*t)})
}

func (s *Server) handleTeamsSubroutes(w http.ResponseWriter, r *http.Request) {
	// Supported:
	// - /v1/teams/{team_id}
	// - /v1/teams/{team_id}/members
	// - /v1/teams/{team_id}/members/{member_id}
	// - /v1/teams/{team_id}/quorum
	// - /v1/teams/{team_id}/quorum/{rule_id}
	// - /v1/teams/{team_id}/runs
	// - /v1/teams/{team_id}/runs/{team_run_id}
	// - /v1/teams/{team_id}/runs/{team_run_id}/approvals
	// - /v1/teams/{team_id}/runs/{team_run_id}/runtime_members

	rest := strings.TrimPrefix(r.URL.Path, "/v1/teams/")
	parts := strings.SplitN(rest, "/", 4)
	if len(parts) < 1 || parts[0] == "" {
		writeErrorJSON(w, "not found", http.StatusNotFound)
		return
	}
	teamID := parts[0]
	if len(parts) == 1 || parts[1] == "" {
		switch r.Method {
		case "GET":
			s.handleTeamGet(w, r, teamID)
		case "PATCH":
			s.handleTeamUpdate(w, r, teamID)
		case "DELETE":
			s.handleTeamDelete(w, r, teamID)
		default:
			writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
		}
		return
	}
	action := parts[1]
	switch action {
	case "members":
		if len(parts) == 2 || parts[2] == "" {
			switch r.Method {
			case "GET":
				s.handleTeamMembersList(w, r, teamID)
			case "POST":
				s.handleTeamMembersCreate(w, r, teamID)
			default:
				writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
			}
			return
		}
		s.handleTeamMemberUpdateOrDelete(w, r, teamID, parts[2])
	case "quorum":
		if len(parts) == 2 || parts[2] == "" {
			switch r.Method {
			case "GET":
				s.handleTeamQuorumList(w, r, teamID)
			case "POST":
				s.handleTeamQuorumCreate(w, r, teamID)
			default:
				writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
			}
			return
		}
		s.handleTeamQuorumUpdateOrDelete(w, r, teamID, parts[2])
	case "runs":
		if len(parts) == 2 || parts[2] == "" {
			switch r.Method {
			case "GET":
				s.handleTeamRunsList(w, r, teamID)
			case "POST":
				s.handleTeamRunCreate(w, r, teamID)
			default:
				writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
			}
			return
		}
		if len(parts) >= 4 && parts[3] != "" {
			switch parts[3] {
			case "approvals":
				if r.Method == "GET" {
					s.handleTeamRunApprovalsList(w, r, teamID, parts[2])
					return
				}
				if r.Method == "POST" {
					s.handleTeamRunApprovalsCreate(w, r, teamID, parts[2])
					return
				}
				writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
				return
			case "runtime_members":
				if r.Method == "PATCH" {
					s.handleTeamRunRuntimeMembersUpdate(w, r, teamID, parts[2])
					return
				}
				writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
				return
			case "cancel":
				if r.Method == "POST" {
					s.handleTeamRunCancel(w, r, teamID, parts[2])
					return
				}
				writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
				return
			default:
				writeErrorJSON(w, "not found", http.StatusNotFound)
				return
			}
		}
		s.handleTeamRunGet(w, r, teamID, parts[2])
	default:
		writeErrorJSON(w, "not found", http.StatusNotFound)
	}
}

func (s *Server) handleTeamGet(w http.ResponseWriter, r *http.Request, teamID string) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if p.AuthKind != "oidc" {
		writeErrorJSON(w, "oidc required", http.StatusForbidden)
		return
	}
	team, ok := s.requireTeamOwner(w, r, p, teamID)
	if !ok {
		return
	}
	writeJSON(w, map[string]any{"ok": true, "team": teamToJSON(*team)})
}

func (s *Server) handleTeamUpdate(w http.ResponseWriter, r *http.Request, teamID string) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if p.AuthKind != "oidc" {
		writeErrorJSON(w, "oidc required", http.StatusForbidden)
		return
	}
	team, ok := s.requireTeamOwner(w, r, p, teamID)
	if !ok {
		return
	}
	body, err := readBodyBounded(r.Body, 1024*1024)
	if err != nil {
		writeErrorJSON(w, "invalid body", http.StatusBadRequest)
		return
	}
	req := struct {
		DisplayName         *string         `json:"display_name"`
		Tags                *[]string       `json:"tags"`
		PolicyRef           *string         `json:"policy_ref"`
		SharedMemoryScopeID *string         `json:"shared_memory_scope_id"`
		Meta                *map[string]any `json:"meta"`
	}{}
	if len(body) > 0 {
		if err := json.Unmarshal(body, &req); err != nil {
			writeErrorJSON(w, "invalid json", http.StatusBadRequest)
			return
		}
	}
	displayName := team.DisplayName
	if req.DisplayName != nil {
		displayName = strings.TrimSpace(*req.DisplayName)
	}
	if displayName == "" {
		writeErrorJSON(w, "missing display_name", http.StatusBadRequest)
		return
	}
	tags := team.Tags()
	if req.Tags != nil {
		tags = *req.Tags
	}
	policyRef := team.PolicyRef
	if req.PolicyRef != nil {
		policyRef = strings.TrimSpace(*req.PolicyRef)
	}
	sharedScope := team.SharedMemoryScopeID
	if req.SharedMemoryScopeID != nil {
		sharedScope = strings.TrimSpace(*req.SharedMemoryScopeID)
	}
	meta := team.Meta()
	if req.Meta != nil {
		meta = *req.Meta
	}
	meta, err = normalizeTeamMetaRoleOverrides(meta)
	if err != nil {
		writeErrorJSON(w, "invalid role_overrides", http.StatusBadRequest)
		return
	}
	updated, err := s.cfg.DB.UpdateTeam(r.Context(), teamID, displayName, tags, policyRef, sharedScope, meta)
	if err != nil {
		writeErrorJSON(w, "update team failed", http.StatusBadRequest)
		return
	}
	writeJSON(w, map[string]any{"ok": true, "team": teamToJSON(*updated)})
}

func (s *Server) handleTeamDelete(w http.ResponseWriter, r *http.Request, teamID string) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if p.AuthKind != "oidc" {
		writeErrorJSON(w, "oidc required", http.StatusForbidden)
		return
	}
	_, ok := s.requireTeamOwner(w, r, p, teamID)
	if !ok {
		return
	}
	if err := s.cfg.DB.DeleteTeam(r.Context(), teamID); err != nil {
		writeErrorJSON(w, "delete team failed", http.StatusInternalServerError)
		return
	}
	writeJSON(w, map[string]any{"ok": true})
}

func (s *Server) handleTeamMembersList(w http.ResponseWriter, r *http.Request, teamID string) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if p.AuthKind != "oidc" {
		writeErrorJSON(w, "oidc required", http.StatusForbidden)
		return
	}
	_, ok := s.requireTeamOwner(w, r, p, teamID)
	if !ok {
		return
	}
	members, err := s.cfg.DB.ListTeamMembers(r.Context(), teamID)
	if err != nil {
		writeErrorJSON(w, "db error", http.StatusInternalServerError)
		return
	}
	out := make([]map[string]any, 0, len(members))
	for _, m := range members {
		out = append(out, teamMemberToJSON(m))
	}
	writeJSON(w, map[string]any{"ok": true, "team_id": teamID, "members": out})
}

func (s *Server) handleTeamMembersCreate(w http.ResponseWriter, r *http.Request, teamID string) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if p.AuthKind != "oidc" {
		writeErrorJSON(w, "oidc required", http.StatusForbidden)
		return
	}
	_, ok := s.requireTeamOwner(w, r, p, teamID)
	if !ok {
		return
	}
	body, err := readBodyBounded(r.Body, 1024*1024)
	if err != nil {
		writeErrorJSON(w, "invalid body", http.StatusBadRequest)
		return
	}
	req := struct {
		MemberID     string         `json:"member_id"`
		DeploymentID string         `json:"deployment_id"`
		AgentID      string         `json:"agent_id"`
		Role         string         `json:"role"`
		Capabilities []string       `json:"capabilities"`
		Status       string         `json:"status"`
		Weight       int            `json:"weight"`
		Meta         map[string]any `json:"meta"`
	}{}
	if len(body) > 0 {
		if err := json.Unmarshal(body, &req); err != nil {
			writeErrorJSON(w, "invalid json", http.StatusBadRequest)
			return
		}
	}
	memberID := strings.TrimSpace(req.MemberID)
	if memberID == "" {
		memberID = "tm_" + newID()[:12]
	}
	role := strings.ToLower(strings.TrimSpace(req.Role))
	if role == "" {
		writeErrorJSON(w, "missing role", http.StatusBadRequest)
		return
	}
	status := strings.ToLower(strings.TrimSpace(req.Status))
	if status == "" {
		status = "active"
	}
	created, err := s.cfg.DB.CreateTeamMember(r.Context(), teamID, memberID, req.DeploymentID, req.AgentID, role, status, req.Capabilities, req.Weight, req.Meta)
	if err != nil {
		writeErrorJSON(w, "create member failed", http.StatusBadRequest)
		return
	}
	writeJSON(w, map[string]any{"ok": true, "member": teamMemberToJSON(*created)})
}

func (s *Server) handleTeamMemberUpdateOrDelete(w http.ResponseWriter, r *http.Request, teamID, memberID string) {
	switch r.Method {
	case "PATCH":
		s.handleTeamMemberUpdate(w, r, teamID, memberID)
	case "DELETE":
		s.handleTeamMemberDelete(w, r, teamID, memberID)
	default:
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
	}
}

func (s *Server) handleTeamMemberUpdate(w http.ResponseWriter, r *http.Request, teamID, memberID string) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if p.AuthKind != "oidc" {
		writeErrorJSON(w, "oidc required", http.StatusForbidden)
		return
	}
	_, ok := s.requireTeamOwner(w, r, p, teamID)
	if !ok {
		return
	}
	existing, err := s.cfg.DB.GetTeamMember(r.Context(), teamID, memberID)
	if err != nil {
		writeErrorJSON(w, "member not found", http.StatusNotFound)
		return
	}
	body, err := readBodyBounded(r.Body, 1024*1024)
	if err != nil {
		writeErrorJSON(w, "invalid body", http.StatusBadRequest)
		return
	}
	req := struct {
		Role         *string         `json:"role"`
		Capabilities *[]string       `json:"capabilities"`
		Status       *string         `json:"status"`
		Weight       *int            `json:"weight"`
		Meta         *map[string]any `json:"meta"`
		DeploymentID *string         `json:"deployment_id"`
		AgentID      *string         `json:"agent_id"`
	}{}
	if len(body) > 0 {
		if err := json.Unmarshal(body, &req); err != nil {
			writeErrorJSON(w, "invalid json", http.StatusBadRequest)
			return
		}
	}
	role := existing.Role
	if req.Role != nil {
		role = strings.ToLower(strings.TrimSpace(*req.Role))
	}
	if role == "" {
		writeErrorJSON(w, "missing role", http.StatusBadRequest)
		return
	}
	status := existing.Status
	if req.Status != nil {
		status = strings.ToLower(strings.TrimSpace(*req.Status))
	}
	if status == "" {
		status = "active"
	}
	caps := existing.Capabilities()
	if req.Capabilities != nil {
		caps = *req.Capabilities
	}
	weight := existing.Weight
	if req.Weight != nil {
		weight = *req.Weight
	}
	meta := existing.Meta()
	if req.Meta != nil {
		meta = *req.Meta
	}
	deploymentID := existing.DeploymentID
	if req.DeploymentID != nil {
		deploymentID = strings.TrimSpace(*req.DeploymentID)
	}
	agentID := existing.AgentID
	if req.AgentID != nil {
		agentID = strings.TrimSpace(*req.AgentID)
	}
	updated, err := s.cfg.DB.UpdateTeamMember(r.Context(), teamID, memberID, deploymentID, agentID, role, status, caps, weight, meta)
	if err != nil {
		writeErrorJSON(w, "update member failed", http.StatusBadRequest)
		return
	}
	writeJSON(w, map[string]any{"ok": true, "member": teamMemberToJSON(*updated)})
}

func (s *Server) handleTeamMemberDelete(w http.ResponseWriter, r *http.Request, teamID, memberID string) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if p.AuthKind != "oidc" {
		writeErrorJSON(w, "oidc required", http.StatusForbidden)
		return
	}
	_, ok := s.requireTeamOwner(w, r, p, teamID)
	if !ok {
		return
	}
	okDelete, err := s.cfg.DB.DeleteTeamMember(r.Context(), teamID, memberID)
	if err != nil {
		writeErrorJSON(w, "delete member failed", http.StatusInternalServerError)
		return
	}
	if !okDelete {
		writeErrorJSON(w, "member not found", http.StatusNotFound)
		return
	}
	writeJSON(w, map[string]any{"ok": true})
}

func (s *Server) handleTeamQuorumList(w http.ResponseWriter, r *http.Request, teamID string) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if p.AuthKind != "oidc" {
		writeErrorJSON(w, "oidc required", http.StatusForbidden)
		return
	}
	_, ok := s.requireTeamOwner(w, r, p, teamID)
	if !ok {
		return
	}
	rules, err := s.cfg.DB.ListTeamQuorumRules(r.Context(), teamID)
	if err != nil {
		writeErrorJSON(w, "db error", http.StatusInternalServerError)
		return
	}
	out := make([]map[string]any, 0, len(rules))
	for _, rule := range rules {
		out = append(out, teamQuorumRuleToJSON(rule))
	}
	writeJSON(w, map[string]any{"ok": true, "team_id": teamID, "rules": out})
}

func (s *Server) handleTeamQuorumCreate(w http.ResponseWriter, r *http.Request, teamID string) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if p.AuthKind != "oidc" {
		writeErrorJSON(w, "oidc required", http.StatusForbidden)
		return
	}
	_, ok := s.requireTeamOwner(w, r, p, teamID)
	if !ok {
		return
	}
	body, err := readBodyBounded(r.Body, 1024*1024)
	if err != nil {
		writeErrorJSON(w, "invalid body", http.StatusBadRequest)
		return
	}
	req := struct {
		RuleID               string         `json:"rule_id"`
		Action               string         `json:"action"`
		ToolNames            []string       `json:"tool_names"`
		MinApprovals         int            `json:"min_approvals"`
		RoleAllowlist        []string       `json:"role_allowlist"`
		RequireDistinctRoles bool           `json:"require_distinct_roles"`
		TimeoutMS            int64          `json:"timeout_ms"`
		QuorumMode           string         `json:"quorum_mode"`
		Meta                 map[string]any `json:"meta"`
	}{}
	if len(body) > 0 {
		if err := json.Unmarshal(body, &req); err != nil {
			writeErrorJSON(w, "invalid json", http.StatusBadRequest)
			return
		}
	}
	ruleID := strings.TrimSpace(req.RuleID)
	if ruleID == "" {
		ruleID = "qr_" + newID()[:12]
	}
	action := strings.TrimSpace(req.Action)
	if action == "" {
		writeErrorJSON(w, "missing action", http.StatusBadRequest)
		return
	}
	if req.MinApprovals < 1 {
		writeErrorJSON(w, "min_approvals must be >= 1", http.StatusBadRequest)
		return
	}
	quorumMode := strings.ToLower(strings.TrimSpace(req.QuorumMode))
	if quorumMode == "" {
		writeErrorJSON(w, "missing quorum_mode", http.StatusBadRequest)
		return
	}
	if quorumMode != "strict" && quorumMode != "best_effort" {
		writeErrorJSON(w, "invalid quorum_mode", http.StatusBadRequest)
		return
	}
	created, err := s.cfg.DB.CreateTeamQuorumRule(r.Context(), teamID, ruleID, action, req.ToolNames, req.MinApprovals, req.RoleAllowlist, req.RequireDistinctRoles, req.TimeoutMS, quorumMode, req.Meta)
	if err != nil {
		writeErrorJSON(w, "create quorum rule failed", http.StatusBadRequest)
		return
	}
	writeJSON(w, map[string]any{"ok": true, "rule": teamQuorumRuleToJSON(*created)})
}

func (s *Server) handleTeamQuorumUpdateOrDelete(w http.ResponseWriter, r *http.Request, teamID, ruleID string) {
	switch r.Method {
	case "PATCH":
		s.handleTeamQuorumUpdate(w, r, teamID, ruleID)
	case "DELETE":
		s.handleTeamQuorumDelete(w, r, teamID, ruleID)
	default:
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
	}
}

func (s *Server) handleTeamQuorumUpdate(w http.ResponseWriter, r *http.Request, teamID, ruleID string) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if p.AuthKind != "oidc" {
		writeErrorJSON(w, "oidc required", http.StatusForbidden)
		return
	}
	_, ok := s.requireTeamOwner(w, r, p, teamID)
	if !ok {
		return
	}
	existing, err := s.cfg.DB.GetTeamQuorumRule(r.Context(), teamID, ruleID)
	if err != nil {
		writeErrorJSON(w, "rule not found", http.StatusNotFound)
		return
	}
	body, err := readBodyBounded(r.Body, 1024*1024)
	if err != nil {
		writeErrorJSON(w, "invalid body", http.StatusBadRequest)
		return
	}
	req := struct {
		Action               *string         `json:"action"`
		ToolNames            *[]string       `json:"tool_names"`
		MinApprovals         *int            `json:"min_approvals"`
		RoleAllowlist        *[]string       `json:"role_allowlist"`
		RequireDistinctRoles *bool           `json:"require_distinct_roles"`
		TimeoutMS            *int64          `json:"timeout_ms"`
		QuorumMode           *string         `json:"quorum_mode"`
		Meta                 *map[string]any `json:"meta"`
	}{}
	if len(body) > 0 {
		if err := json.Unmarshal(body, &req); err != nil {
			writeErrorJSON(w, "invalid json", http.StatusBadRequest)
			return
		}
	}
	action := existing.Action
	if req.Action != nil {
		action = strings.TrimSpace(*req.Action)
	}
	if action == "" {
		writeErrorJSON(w, "missing action", http.StatusBadRequest)
		return
	}
	minApprovals := existing.MinApprovals
	if req.MinApprovals != nil {
		minApprovals = *req.MinApprovals
	}
	if minApprovals < 1 {
		writeErrorJSON(w, "min_approvals must be >= 1", http.StatusBadRequest)
		return
	}
	toolNames := existing.ToolNames()
	if req.ToolNames != nil {
		toolNames = *req.ToolNames
	}
	roleAllowlist := existing.RoleAllowlist()
	if req.RoleAllowlist != nil {
		roleAllowlist = *req.RoleAllowlist
	}
	requireDistinct := existing.RequireDistinctRoles
	if req.RequireDistinctRoles != nil {
		requireDistinct = *req.RequireDistinctRoles
	}
	timeoutMS := existing.TimeoutMS
	if req.TimeoutMS != nil {
		timeoutMS = *req.TimeoutMS
	}
	quorumMode := existing.QuorumMode
	if req.QuorumMode != nil {
		quorumMode = strings.ToLower(strings.TrimSpace(*req.QuorumMode))
	}
	if quorumMode == "" {
		writeErrorJSON(w, "missing quorum_mode", http.StatusBadRequest)
		return
	}
	if quorumMode != "strict" && quorumMode != "best_effort" {
		writeErrorJSON(w, "invalid quorum_mode", http.StatusBadRequest)
		return
	}
	meta := existing.Meta()
	if req.Meta != nil {
		meta = *req.Meta
	}
	updated, err := s.cfg.DB.UpdateTeamQuorumRule(r.Context(), teamID, ruleID, action, toolNames, minApprovals, roleAllowlist, requireDistinct, timeoutMS, quorumMode, meta)
	if err != nil {
		writeErrorJSON(w, "update quorum rule failed", http.StatusBadRequest)
		return
	}
	writeJSON(w, map[string]any{"ok": true, "rule": teamQuorumRuleToJSON(*updated)})
}

func (s *Server) handleTeamQuorumDelete(w http.ResponseWriter, r *http.Request, teamID, ruleID string) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if p.AuthKind != "oidc" {
		writeErrorJSON(w, "oidc required", http.StatusForbidden)
		return
	}
	_, ok := s.requireTeamOwner(w, r, p, teamID)
	if !ok {
		return
	}
	okDelete, err := s.cfg.DB.DeleteTeamQuorumRule(r.Context(), teamID, ruleID)
	if err != nil {
		writeErrorJSON(w, "delete quorum rule failed", http.StatusInternalServerError)
		return
	}
	if !okDelete {
		writeErrorJSON(w, "rule not found", http.StatusNotFound)
		return
	}
	writeJSON(w, map[string]any{"ok": true})
}

func (s *Server) handleTeamRunCreate(w http.ResponseWriter, r *http.Request, teamID string) {
	if r.Method != "POST" {
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if p.AuthKind != "oidc" {
		writeErrorJSON(w, "oidc required", http.StatusForbidden)
		return
	}
	team, ok := s.requireTeamOwner(w, r, p, teamID)
	if !ok {
		return
	}
	body, err := readBodyBounded(r.Body, 1024*1024)
	if err != nil {
		writeErrorJSON(w, "invalid body", http.StatusBadRequest)
		return
	}
	raw := map[string]json.RawMessage{}
	if len(body) > 0 {
		if err := json.Unmarshal(body, &raw); err != nil {
			writeErrorJSON(w, "invalid json", http.StatusBadRequest)
			return
		}
	}
	runRaw, ok := raw["run"]
	if !ok || len(runRaw) == 0 || string(runRaw) == "null" {
		writeErrorJSON(w, "missing run", http.StatusBadRequest)
		return
	}
	runMap := map[string]any{}
	if err := json.Unmarshal(runRaw, &runMap); err != nil || len(runMap) == 0 {
		writeErrorJSON(w, "invalid run", http.StatusBadRequest)
		return
	}
	teamMeta := map[string]any{}
	if v, ok := raw["team"]; ok && len(v) > 0 && string(v) != "null" {
		_ = json.Unmarshal(v, &teamMeta)
	}
	traceID := traceIDFromContext(r.Context())

	options, err := parseTeamRunOptions(teamMeta)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusBadRequest)
		return
	}
	overrides, err := parseTeamRunOverrides(teamMeta)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusBadRequest)
		return
	}
	roleOverridesProvided := false
	if _, ok := teamMeta["role_overrides"]; ok {
		roleOverridesProvided = true
	}
	roleOverrides, err := parseRoleOverrides(teamMeta["role_overrides"])
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusBadRequest)
		return
	}
	if !roleOverridesProvided && team != nil {
		if teamMetaDefaults := team.Meta(); len(teamMetaDefaults) > 0 {
			if rawDefaults, ok := teamMetaDefaults["role_overrides"]; ok {
				defaults, err := parseRoleOverrides(rawDefaults)
				if err != nil {
					writeErrorJSON(w, "invalid team role_overrides", http.StatusBadRequest)
					return
				}
				if len(defaults) > 0 {
					roleOverrides = defaults
					teamMeta["role_overrides"] = defaults
				}
			}
		}
	}
	teamMeta["run_overrides_mode"] = overrides.Mode
	if overrides.Mode != "explicit" {
		delete(teamMeta, "member_overrides")
	} else if len(overrides.MemberOverrides) > 0 {
		teamMeta["member_overrides"] = overrides.MemberOverrides
	}
	if len(roleOverrides) > 0 {
		teamMeta["role_overrides"] = roleOverrides
	} else {
		delete(teamMeta, "role_overrides")
	}
	quorumPolicyMode, err := parseTeamRunQuorumPolicy(teamMeta)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusBadRequest)
		return
	}
	approvals, err := parseTeamRunApprovals(teamMeta)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusBadRequest)
		return
	}
	teamRunRules := []db.TeamQuorumRule{}
	if quorumPolicyMode != "off" || len(approvals) > 0 {
		rules, err := s.cfg.DB.ListTeamQuorumRules(r.Context(), teamID)
		if err != nil {
			writeErrorJSON(w, "db error", http.StatusInternalServerError)
			return
		}
		teamRunRules = filterTeamRunRules(rules)
		if len(approvals) > 0 && len(teamRunRules) == 0 {
			writeErrorJSON(w, "no quorum rules configured", http.StatusBadRequest)
			return
		}
	}
	members, err := s.cfg.DB.ListTeamMembers(r.Context(), teamID)
	if err != nil {
		writeErrorJSON(w, "db error", http.StatusInternalServerError)
		return
	}
	membersByID := map[string]db.TeamMember{}
	usedMemberIDs := map[string]bool{}
	for _, m := range members {
		membersByID[m.MemberID] = m
		usedMemberIDs[m.MemberID] = true
	}
	runtimeInputs, err := parseTeamRunRuntimeMembers(teamMeta)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusBadRequest)
		return
	}
	runtimeMembers, runtimeMembersJSON, err := buildRuntimeMembers(runtimeInputs, teamID, usedMemberIDs)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusBadRequest)
		return
	}
	if len(runtimeMembersJSON) > 0 {
		teamMeta["runtime_members"] = runtimeMembersJSON
	} else {
		delete(teamMeta, "runtime_members")
	}
	var quorumEval *teamRunQuorumEval
	if quorumPolicyMode != "off" {
		eval, err := evaluateTeamRunQuorum(teamRunRules, approvals, membersByID)
		if err != nil {
			writeErrorJSON(w, err.Error(), http.StatusBadRequest)
			return
		}
		if len(eval.Rules) > 0 {
			if !eval.StrictOK {
				w.WriteHeader(http.StatusConflict)
				writeJSON(w, map[string]any{
					"ok":     false,
					"error":  "quorum approvals required",
					"quorum": eval.toJSON(),
				})
				return
			}
			teamMeta["quorum_eval"] = eval.toJSON()
			quorumEval = &eval
		}
	}
	persistentMembers := filterTeamRunMembers(members, options.Role, options.Roles)
	runtimeMembers = filterTeamRunMembers(runtimeMembers, options.Role, options.Roles)
	runMembers := append(persistentMembers, runtimeMembers...)
	if len(runMembers) == 0 {
		writeErrorJSON(w, "no eligible team members", http.StatusBadRequest)
		return
	}
	for _, m := range runMembers {
		if strings.TrimSpace(m.AgentID) == "" {
			writeErrorJSON(w, "team member missing agent_id", http.StatusBadRequest)
			return
		}
		if ok, err := s.canAccessAgent(r.Context(), p, m.AgentID); err != nil {
			writeErrorJSON(w, "db error", http.StatusInternalServerError)
			return
		} else if !ok {
			writeErrorJSON(w, "forbidden", http.StatusForbidden)
			return
		}
	}

	if traceID != "" {
		if _, ok := runMap["trace_id"]; !ok {
			runMap["trace_id"] = traceID
		}
	}

	memberOverridesApplied := map[string]map[string]any{}
	roleOverridesApplied := map[string]map[string]any{}
	memberRunBodies := make([][]byte, 0, len(runMembers))
	for _, member := range runMembers {
		runForMember := map[string]any{}
		for k, v := range runMap {
			runForMember[k] = v
		}
		roleKey := strings.ToLower(strings.TrimSpace(member.Role))
		if roleKey != "" && len(roleOverrides) > 0 {
			if overridesForRole, ok := roleOverrides[roleKey]; ok && len(overridesForRole) > 0 {
				for k, v := range overridesForRole {
					runForMember[k] = v
				}
				roleOverridesApplied[member.MemberID] = overridesForRole
			}
		}
		var overridesForMember map[string]any
		switch overrides.Mode {
		case "member_meta":
			overridesForMember = memberMetaRunOverrides(member.Meta())
		case "explicit":
			if overrides.MemberOverrides != nil {
				overridesForMember = overrides.MemberOverrides[member.MemberID]
			}
		}
		if len(overridesForMember) > 0 {
			for k, v := range overridesForMember {
				runForMember[k] = v
			}
			memberOverridesApplied[member.MemberID] = overridesForMember
		}
		memberRunBodies = append(memberRunBodies, mustJSON(runForMember))
	}
	if len(memberOverridesApplied) > 0 {
		teamMeta["member_overrides_applied"] = memberOverridesApplied
	}
	if len(roleOverridesApplied) > 0 {
		teamMeta["role_overrides_applied"] = roleOverridesApplied
	}
	teamMeta["mode"] = options.Mode

	teamRunID := "tr_" + newID()[:12]
	if options.Mode == "async" {
		resp, err := s.executeTeamRunAsync(
			r.Context(),
			p,
			teamID,
			teamRunID,
			runMap,
			teamMeta,
			runMembers,
			memberRunBodies,
			options,
			teamRunRules,
			approvals,
			membersByID,
			quorumEval,
			traceID,
		)
		if err != nil {
			writeErrorJSON(w, err.Error(), http.StatusBadRequest)
			return
		}
		writeJSON(w, resp)
		return
	}

	runPayload := map[string]any{
		"run":  runMap,
		"team": teamMeta,
	}
	run, err := s.cfg.DB.CreateTeamRun(r.Context(), teamRunID, teamID, "running", p.Sub, mustJSON(runPayload))
	if err != nil {
		writeErrorJSON(w, "create team run failed", http.StatusBadRequest)
		return
	}
	publishTeamRunCreated(s.cfg.Events, p.Sub, teamID, teamRunID, "running", options.Mode, p.Sub, run.CreatedAt.UnixMilli(), nil, traceID)
	if len(approvals) > 0 {
		if err := s.persistTeamRunApprovals(r.Context(), teamID, teamRunID, teamRunRules, approvals, membersByID, p.Sub); err != nil {
			_ = s.cfg.DB.UpdateTeamRunStatus(r.Context(), teamID, teamRunID, "failed")
			publishTeamRunStatus(s.cfg.Events, p.Sub, teamID, teamRunID, "failed", options.Mode, run.CreatedAt.UnixMilli(), nil, traceID)
			writeErrorJSON(w, err.Error(), http.StatusBadRequest)
			return
		}
	}
	if len(teamRunRules) > 0 {
		publishTeamQuorumRequest(s.cfg.Events, p.Sub, teamID, teamRunID, teamRunRules, traceID)
		if quorumEval != nil {
			publishTeamQuorumResult(s.cfg.Events, p.Sub, teamID, teamRunID, *quorumEval, traceID)
		}
	}

	tasks := make([]agentTaskPrepared, 0, len(runMembers))
	for i, member := range runMembers {
		body := mustJSON(runMap)
		if i < len(memberRunBodies) {
			body = memberRunBodies[i]
		}
		tasks = append(tasks, agentTaskPrepared{
			TaskID:       "member_" + itoa(i),
			AgentID:      member.AgentID,
			DeploymentID: member.DeploymentID,
			Method:       "POST",
			Path:         "/api/v1/run",
			Query:        "",
			Headers:      map[string]string{},
			Body:         body,
		})
	}
	results := s.executeAgentTasks(r.Context(), p, tasks, options.MaxConcurrency, options.TimeoutMS, traceID)
	allOK := true
	for _, r := range results {
		if !r.OK {
			allOK = false
			break
		}
	}
	status := "succeeded"
	if !allOK {
		status = "failed"
	}
	if err := s.cfg.DB.UpdateTeamRunStatus(r.Context(), teamID, teamRunID, status); err != nil {
		writeErrorJSON(w, "update team run failed", http.StatusInternalServerError)
		return
	}
	run, err = s.cfg.DB.GetTeamRun(r.Context(), teamID, teamRunID)
	if err != nil {
		writeErrorJSON(w, "team run not found", http.StatusNotFound)
		return
	}
	publishTeamRunStatus(s.cfg.Events, p.Sub, teamID, teamRunID, run.Status, options.Mode, run.CreatedAt.UnixMilli(), nil, traceID)
	writeJSON(w, map[string]any{
		"ok":              true,
		"team_id":         run.TeamID,
		"team_run_id":     run.TeamRunID,
		"status":          run.Status,
		"mode":            options.Mode,
		"created_unix_ms": run.CreatedAt.UnixMilli(),
	})
}

func (s *Server) handleTeamRunsList(w http.ResponseWriter, r *http.Request, teamID string) {
	if r.Method != "GET" {
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if p.AuthKind != "oidc" {
		writeErrorJSON(w, "oidc required", http.StatusForbidden)
		return
	}
	_, ok := s.requireTeamOwner(w, r, p, teamID)
	if !ok {
		return
	}
	limit := 25
	if v := strings.TrimSpace(r.URL.Query().Get("limit")); v != "" {
		if n, ok := parseIntBounded(v, 1, 200); ok {
			limit = n
		}
	}
	offset := 0
	if v := strings.TrimSpace(r.URL.Query().Get("offset")); v != "" {
		if n, ok := parseIntBounded(v, 0, 10_000); ok {
			offset = n
		}
	}
	status := strings.TrimSpace(r.URL.Query().Get("status"))
	if status != "" {
		status = strings.ToLower(status)
	}
	rows, err := s.cfg.DB.ListTeamRuns(r.Context(), teamID, limit, offset, status)
	if err != nil {
		writeErrorJSON(w, "db error", http.StatusInternalServerError)
		return
	}
	out := make([]map[string]any, 0, len(rows))
	for _, row := range rows {
		out = append(out, teamRunSummaryToJSON(row))
	}
	resp := map[string]any{
		"ok":      true,
		"team_id": teamID,
		"limit":   limit,
		"offset":  offset,
		"runs":    out,
	}
	if status != "" {
		resp["status"] = status
	}
	writeJSON(w, resp)
}

func (s *Server) handleTeamRunGet(w http.ResponseWriter, r *http.Request, teamID, teamRunID string) {
	if r.Method != "GET" {
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if p.AuthKind != "oidc" {
		writeErrorJSON(w, "oidc required", http.StatusForbidden)
		return
	}
	_, ok := s.requireTeamOwner(w, r, p, teamID)
	if !ok {
		return
	}
	run, err := s.cfg.DB.GetTeamRun(r.Context(), teamID, teamRunID)
	if err != nil {
		writeErrorJSON(w, "team run not found", http.StatusNotFound)
		return
	}
	resp, err := s.teamRunStatusResponse(r.Context(), p, run)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSON(w, resp)
}

func (s *Server) handleTeamRunCancel(w http.ResponseWriter, r *http.Request, teamID, teamRunID string) {
	if r.Method != "POST" {
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if p.AuthKind != "oidc" {
		writeErrorJSON(w, "oidc required", http.StatusForbidden)
		return
	}
	_, ok := s.requireTeamOwner(w, r, p, teamID)
	if !ok {
		return
	}
	run, err := s.cfg.DB.GetTeamRun(r.Context(), teamID, teamRunID)
	if err != nil {
		writeErrorJSON(w, "team run not found", http.StatusNotFound)
		return
	}

	runPayload := map[string]any{}
	teamMeta := map[string]any{}
	if len(run.RunJSON) > 0 {
		if err := json.Unmarshal(run.RunJSON, &runPayload); err != nil {
			writeErrorJSON(w, "invalid team run payload", http.StatusInternalServerError)
			return
		}
		if teamRaw, ok := runPayload["team"].(map[string]any); ok {
			teamMeta = teamRaw
		}
	}
	mode := "sync"
	if v, ok := teamMeta["mode"].(string); ok {
		mode = strings.ToLower(strings.TrimSpace(v))
		if mode == "" {
			mode = "sync"
		}
	}
	if mode != "async" {
		writeErrorJSON(w, "team run is not async", http.StatusBadRequest)
		return
	}
	options := teamRunOptions{MaxConcurrency: 4}
	if parsed, err := parseTeamRunOptions(teamMeta); err == nil {
		options = parsed
	}
	traceID := traceIDFromContext(r.Context())
	prevStatus := run.Status
	if _, err := s.cancelTeamRunJobs(r.Context(), p, run, runPayload, teamMeta, options.MaxConcurrency, traceID); err != nil {
		writeErrorJSON(w, err.Error(), http.StatusBadRequest)
		return
	}
	if run.Status != prevStatus {
		publishTeamRunStatus(s.cfg.Events, p.Sub, teamID, teamRunID, run.Status, mode, run.CreatedAt.UnixMilli(), teamRunMemberJobSummary(teamMeta), traceID)
	}
	resp, err := s.teamRunStatusResponse(r.Context(), p, run)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSON(w, resp)
}

func (s *Server) handleTeamRunRuntimeMembersUpdate(w http.ResponseWriter, r *http.Request, teamID, teamRunID string) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if p.AuthKind != "oidc" {
		writeErrorJSON(w, "oidc required", http.StatusForbidden)
		return
	}
	if _, ok := s.requireTeamOwner(w, r, p, teamID); !ok {
		return
	}
	run, err := s.cfg.DB.GetTeamRun(r.Context(), teamID, teamRunID)
	if err != nil {
		writeErrorJSON(w, "team run not found", http.StatusNotFound)
		return
	}
	body, err := readBodyBounded(r.Body, 1024*1024)
	if err != nil {
		writeErrorJSON(w, "invalid body", http.StatusBadRequest)
		return
	}
	if len(body) == 0 {
		writeErrorJSON(w, "missing body", http.StatusBadRequest)
		return
	}
	var raw any
	if err := json.Unmarshal(body, &raw); err != nil {
		writeErrorJSON(w, "invalid json", http.StatusBadRequest)
		return
	}
	mode := "replace"
	var items []any
	switch t := raw.(type) {
	case []any:
		items = t
	case map[string]any:
		if v, ok := t["mode"]; ok {
			if s, ok := v.(string); ok {
				mode = strings.ToLower(strings.TrimSpace(s))
			} else {
				writeErrorJSON(w, "mode must be string", http.StatusBadRequest)
				return
			}
		}
		if v, ok := t["runtime_members"]; ok {
			if v == nil {
				items = []any{}
			} else if arr, ok := v.([]any); ok {
				items = arr
			} else {
				writeErrorJSON(w, "runtime_members must be array", http.StatusBadRequest)
				return
			}
		} else {
			writeErrorJSON(w, "runtime_members required", http.StatusBadRequest)
			return
		}
	default:
		writeErrorJSON(w, "invalid payload", http.StatusBadRequest)
		return
	}
	if mode == "" {
		mode = "replace"
	}
	if mode != "replace" && mode != "merge" {
		writeErrorJSON(w, "invalid mode (replace|merge)", http.StatusBadRequest)
		return
	}

	incomingInputs, err := parseTeamRunRuntimeMembers(map[string]any{"runtime_members": items})
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusBadRequest)
		return
	}
	var runPayload map[string]any
	if len(run.RunJSON) > 0 {
		if err := json.Unmarshal(run.RunJSON, &runPayload); err != nil {
			writeErrorJSON(w, "invalid stored run payload", http.StatusInternalServerError)
			return
		}
	}
	if runPayload == nil {
		runPayload = map[string]any{}
	}
	teamMeta, _ := runPayload["team"].(map[string]any)
	if teamMeta == nil {
		teamMeta = map[string]any{}
	}
	var existingInputs []teamRuntimeMemberInput
	if mode == "merge" {
		if rawMembers, ok := teamMeta["runtime_members"]; ok && rawMembers != nil {
			if arr, ok := rawMembers.([]any); ok {
				existingInputs, err = parseTeamRunRuntimeMembers(map[string]any{"runtime_members": arr})
				if err != nil {
					writeErrorJSON(w, err.Error(), http.StatusBadRequest)
					return
				}
			}
		}
	}
	mergedInputs := incomingInputs
	if mode == "merge" {
		mergedInputs = mergeRuntimeMemberInputs(existingInputs, incomingInputs)
	}

	members, err := s.cfg.DB.ListTeamMembers(r.Context(), teamID)
	if err != nil {
		writeErrorJSON(w, "db error", http.StatusInternalServerError)
		return
	}
	usedIDs := map[string]bool{}
	for _, m := range members {
		if m.MemberID != "" {
			usedIDs[m.MemberID] = true
		}
	}
	runtimeMembers, runtimeMembersJSON, err := buildRuntimeMembers(mergedInputs, teamID, usedIDs)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusBadRequest)
		return
	}
	for _, m := range runtimeMembers {
		if strings.TrimSpace(m.AgentID) == "" {
			writeErrorJSON(w, "runtime member missing agent_id", http.StatusBadRequest)
			return
		}
		if ok, err := s.canAccessAgent(r.Context(), p, m.AgentID); err != nil {
			writeErrorJSON(w, "db error", http.StatusInternalServerError)
			return
		} else if !ok {
			writeErrorJSON(w, "forbidden", http.StatusForbidden)
			return
		}
	}
	if len(runtimeMembersJSON) > 0 {
		teamMeta["runtime_members"] = runtimeMembersJSON
	} else {
		delete(teamMeta, "runtime_members")
	}
	teamMeta["runtime_members_updated_unix_ms"] = time.Now().UTC().UnixMilli()
	runPayload["team"] = teamMeta
	if err := s.cfg.DB.UpdateTeamRunPayload(r.Context(), teamID, teamRunID, mustJSON(runPayload)); err != nil {
		writeErrorJSON(w, "update team run failed", http.StatusInternalServerError)
		return
	}
	run.RunJSON = mustJSON(runPayload)
	publishTeamRuntimeMembersUpdated(s.cfg.Events, p.Sub, teamID, teamRunID, runtimeMembersJSON, traceIDFromContext(r.Context()))
	resp, err := s.teamRunStatusResponse(r.Context(), p, run)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusInternalServerError)
		return
	}
	if len(runtimeMembersJSON) == 0 {
		delete(resp, "runtime_members")
	}
	writeJSON(w, resp)
}

func (s *Server) teamRunStatusResponse(ctx context.Context, p *Principal, run *db.TeamRun) (map[string]any, error) {
	if run == nil {
		return nil, errors.New("missing team run")
	}
	var runPayload map[string]any
	teamMeta := map[string]any{}
	if len(run.RunJSON) > 0 {
		if err := json.Unmarshal(run.RunJSON, &runPayload); err == nil {
			if teamRaw, ok := runPayload["team"].(map[string]any); ok {
				teamMeta = teamRaw
			}
		}
	}
	mode := "sync"
	if v, ok := teamMeta["mode"].(string); ok {
		mode = strings.ToLower(strings.TrimSpace(v))
		if mode == "" {
			mode = "sync"
		}
	}
	if mode == "async" && runPayload != nil && len(teamMeta) > 0 {
		if _, err := s.reconcileTeamRunJobs(ctx, p, run, runPayload, teamMeta); err != nil {
			return nil, err
		}
	}
	var runtimeMembers any
	if raw, ok := teamMeta["runtime_members"]; ok {
		runtimeMembers = raw
	}
	var memberOverridesApplied any
	if raw, ok := teamMeta["member_overrides_applied"]; ok {
		memberOverridesApplied = raw
	}
	var roleOverridesApplied any
	if raw, ok := teamMeta["role_overrides_applied"]; ok {
		roleOverridesApplied = raw
	}
	var runOverridesMode any
	if raw, ok := teamMeta["run_overrides_mode"]; ok {
		runOverridesMode = raw
	}
	var memberJobs any
	if raw, ok := teamMeta["member_jobs"]; ok {
		memberJobs = raw
	}
	var dispatchErrors any
	if raw, ok := teamMeta["dispatch_errors"]; ok {
		dispatchErrors = raw
	}
	var cancelRequested any
	if raw, ok := teamMeta["cancel_requested_unix_ms"]; ok {
		cancelRequested = raw
	}
	var cancelResults any
	if raw, ok := teamMeta["cancel_results"]; ok {
		cancelResults = raw
	}
	summary := teamRunMemberJobSummary(teamMeta)
	members, err := s.cfg.DB.ListTeamMembers(ctx, run.TeamID)
	if err != nil {
		return nil, err
	}
	outMembers := make([]map[string]any, 0, len(members))
	for _, m := range members {
		outMembers = append(outMembers, teamMemberToJSON(m))
	}
	resp := map[string]any{
		"ok":              true,
		"team_id":         run.TeamID,
		"team_run_id":     run.TeamRunID,
		"status":          run.Status,
		"mode":            mode,
		"created_unix_ms": run.CreatedAt.UnixMilli(),
		"members":         outMembers,
	}
	if runtimeMembers != nil {
		resp["runtime_members"] = runtimeMembers
	}
	if memberOverridesApplied != nil {
		resp["member_overrides_applied"] = memberOverridesApplied
	}
	if roleOverridesApplied != nil {
		resp["role_overrides_applied"] = roleOverridesApplied
	}
	if runOverridesMode != nil {
		resp["run_overrides_mode"] = runOverridesMode
	}
	if memberJobs != nil {
		resp["member_jobs"] = memberJobs
	}
	if dispatchErrors != nil {
		resp["dispatch_errors"] = dispatchErrors
	}
	if summary != nil {
		resp["member_job_summary"] = summary
	}
	if cancelRequested != nil {
		resp["cancel_requested_unix_ms"] = cancelRequested
	}
	if cancelResults != nil {
		resp["cancel_results"] = cancelResults
	}
	return resp, nil
}

func (s *Server) handleTeamRunApprovalsList(w http.ResponseWriter, r *http.Request, teamID, teamRunID string) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if p.AuthKind != "oidc" {
		writeErrorJSON(w, "oidc required", http.StatusForbidden)
		return
	}
	_, ok := s.requireTeamOwner(w, r, p, teamID)
	if !ok {
		return
	}
	if _, err := s.cfg.DB.GetTeamRun(r.Context(), teamID, teamRunID); err != nil {
		writeErrorJSON(w, "team run not found", http.StatusNotFound)
		return
	}
	approvals, err := s.cfg.DB.ListTeamRunApprovals(r.Context(), teamID, teamRunID)
	if err != nil {
		writeErrorJSON(w, "db error", http.StatusInternalServerError)
		return
	}
	out := make([]map[string]any, 0, len(approvals))
	for _, approval := range approvals {
		out = append(out, teamRunApprovalToJSON(approval))
	}
	writeJSON(w, map[string]any{
		"ok":          true,
		"team_id":     teamID,
		"team_run_id": teamRunID,
		"approvals":   out,
	})
}

func (s *Server) handleTeamRunApprovalsCreate(w http.ResponseWriter, r *http.Request, teamID, teamRunID string) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if p.AuthKind != "oidc" {
		writeErrorJSON(w, "oidc required", http.StatusForbidden)
		return
	}
	_, ok := s.requireTeamOwner(w, r, p, teamID)
	if !ok {
		return
	}
	if _, err := s.cfg.DB.GetTeamRun(r.Context(), teamID, teamRunID); err != nil {
		writeErrorJSON(w, "team run not found", http.StatusNotFound)
		return
	}
	body, err := readBodyBounded(r.Body, 1024*1024)
	if err != nil {
		writeErrorJSON(w, "invalid body", http.StatusBadRequest)
		return
	}
	approvals, err := parseTeamRunApprovalsRequest(body)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusBadRequest)
		return
	}
	if len(approvals) == 0 {
		writeErrorJSON(w, "missing approvals", http.StatusBadRequest)
		return
	}
	members, err := s.cfg.DB.ListTeamMembers(r.Context(), teamID)
	if err != nil {
		writeErrorJSON(w, "db error", http.StatusInternalServerError)
		return
	}
	membersByID := map[string]db.TeamMember{}
	for _, m := range members {
		membersByID[m.MemberID] = m
	}
	rules, err := s.cfg.DB.ListTeamQuorumRules(r.Context(), teamID)
	if err != nil {
		writeErrorJSON(w, "db error", http.StatusInternalServerError)
		return
	}
	teamRunRules := filterTeamRunRules(rules)
	if len(teamRunRules) == 0 {
		writeErrorJSON(w, "no quorum rules configured", http.StatusBadRequest)
		return
	}
	if err := s.persistTeamRunApprovals(r.Context(), teamID, teamRunID, teamRunRules, approvals, membersByID, p.Sub); err != nil {
		writeErrorJSON(w, err.Error(), http.StatusBadRequest)
		return
	}
	stored, err := s.cfg.DB.ListTeamRunApprovals(r.Context(), teamID, teamRunID)
	if err != nil {
		writeErrorJSON(w, "db error", http.StatusInternalServerError)
		return
	}
	if eval, err := evaluateTeamRunQuorum(teamRunRules, approvalsToTeamRunApprovals(stored), membersByID); err == nil {
		publishTeamQuorumResult(s.cfg.Events, p.Sub, teamID, teamRunID, eval, traceIDFromContext(r.Context()))
	}
	out := make([]map[string]any, 0, len(stored))
	for _, approval := range stored {
		out = append(out, teamRunApprovalToJSON(approval))
	}
	writeJSON(w, map[string]any{
		"ok":          true,
		"team_id":     teamID,
		"team_run_id": teamRunID,
		"approvals":   out,
	})
}

func (s *Server) requireTeamOwner(w http.ResponseWriter, r *http.Request, p *Principal, teamID string) (*db.Team, bool) {
	teamID = strings.TrimSpace(teamID)
	if teamID == "" {
		writeErrorJSON(w, "missing team_id", http.StatusBadRequest)
		return nil, false
	}
	team, err := s.cfg.DB.GetTeam(r.Context(), teamID)
	if err != nil {
		writeErrorJSON(w, "team not found", http.StatusNotFound)
		return nil, false
	}
	if !p.Admin && p.Sub != team.OwnerSub {
		writeErrorJSON(w, "not owner", http.StatusForbidden)
		return nil, false
	}
	return team, true
}

func teamRunSummaryToJSON(run db.TeamRun) map[string]any {
	out := map[string]any{
		"team_run_id":     run.TeamRunID,
		"team_id":         run.TeamID,
		"status":          run.Status,
		"created_by":      run.CreatedBy,
		"created_unix_ms": run.CreatedAt.UnixMilli(),
	}
	if len(run.RunJSON) == 0 {
		return out
	}
	var payload map[string]any
	if err := json.Unmarshal(run.RunJSON, &payload); err != nil || payload == nil {
		return out
	}
	if rawTeam, ok := payload["team"]; ok {
		if teamMeta, ok := rawTeam.(map[string]any); ok {
			if modeRaw, ok := teamMeta["mode"]; ok {
				if mode, ok := modeRaw.(string); ok {
					mode = strings.ToLower(strings.TrimSpace(mode))
					if mode != "" {
						out["mode"] = mode
					}
				}
			}
			if summary := teamRunMemberJobSummary(teamMeta); summary != nil {
				out["member_job_summary"] = summary
			}
		}
	}
	return out
}

func teamToJSON(t db.Team) map[string]any {
	out := map[string]any{
		"team_id":         t.TeamID,
		"owner_sub":       t.OwnerSub,
		"display_name":    t.DisplayName,
		"created_unix_ms": t.CreatedAt.UnixMilli(),
		"tags":            t.Tags(),
		"meta":            t.Meta(),
	}
	if t.PolicyRef != "" {
		out["policy_ref"] = t.PolicyRef
	}
	if t.SharedMemoryScopeID != "" {
		out["shared_memory_scope_id"] = t.SharedMemoryScopeID
	}
	return out
}

func teamMemberToJSON(m db.TeamMember) map[string]any {
	out := map[string]any{
		"member_id":       m.MemberID,
		"team_id":         m.TeamID,
		"role":            m.Role,
		"status":          m.Status,
		"weight":          m.Weight,
		"created_unix_ms": m.CreatedAt.UnixMilli(),
		"capabilities":    m.Capabilities(),
		"meta":            m.Meta(),
	}
	if m.DeploymentID != "" {
		out["deployment_id"] = m.DeploymentID
	}
	if m.AgentID != "" {
		out["agent_id"] = m.AgentID
	}
	return out
}

func teamQuorumRuleToJSON(r db.TeamQuorumRule) map[string]any {
	out := map[string]any{
		"rule_id":                r.RuleID,
		"team_id":                r.TeamID,
		"action":                 r.Action,
		"tool_names":             r.ToolNames(),
		"min_approvals":          r.MinApprovals,
		"role_allowlist":         r.RoleAllowlist(),
		"require_distinct_roles": r.RequireDistinctRoles,
		"timeout_ms":             r.TimeoutMS,
		"quorum_mode":            r.QuorumMode,
		"created_unix_ms":        r.CreatedAt.UnixMilli(),
		"meta":                   r.Meta(),
	}
	return out
}

func teamRunApprovalToJSON(a db.TeamRunApproval) map[string]any {
	out := map[string]any{
		"approval_id":     a.ApprovalID,
		"team_run_id":     a.TeamRunID,
		"team_id":         a.TeamID,
		"rule_id":         a.RuleID,
		"member_id":       a.MemberID,
		"role":            a.Role,
		"decision":        a.Decision,
		"created_unix_ms": a.CreatedAt.UnixMilli(),
	}
	if a.Reason != "" {
		out["reason"] = a.Reason
	}
	if a.CreatedBy != "" {
		out["created_by"] = a.CreatedBy
	}
	return out
}

func approvalsToTeamRunApprovals(rows []db.TeamRunApproval) []teamRunApproval {
	out := make([]teamRunApproval, 0, len(rows))
	for _, row := range rows {
		decision := strings.ToLower(strings.TrimSpace(row.Decision))
		out = append(out, teamRunApproval{
			RuleID:   row.RuleID,
			MemberID: row.MemberID,
			Decision: decision,
			Reason:   row.Reason,
		})
	}
	return out
}

func publishTeamQuorumRequest(hub *events.Hub, userSub, teamID, teamRunID string, rules []db.TeamQuorumRule, traceID string) {
	if hub == nil || userSub == "" {
		return
	}
	for _, rule := range rules {
		ev := events.Event{
			Type:    "team_quorum_request",
			UserSub: userSub,
			TraceID: traceID,
			Payload: map[string]any{
				"team_id":       teamID,
				"team_run_id":   teamRunID,
				"rule_id":       rule.RuleID,
				"action":        rule.Action,
				"min_approvals": rule.MinApprovals,
				"quorum_mode":   rule.QuorumMode,
			},
		}
		hub.PublishTo([]string{userSub}, ev)
	}
}

func publishTeamQuorumResult(hub *events.Hub, userSub, teamID, teamRunID string, eval teamRunQuorumEval, traceID string) {
	if hub == nil || userSub == "" {
		return
	}
	for _, rule := range eval.Rules {
		ok := rule.Missing == 0
		decision := "approve"
		if !ok {
			if strings.ToLower(strings.TrimSpace(rule.QuorumMode)) == "best_effort" {
				decision = "best_effort"
			} else {
				decision = "deny"
			}
		}
		payload := map[string]any{
			"team_id":            teamID,
			"team_run_id":        teamRunID,
			"rule_id":            rule.RuleID,
			"decision":           decision,
			"approvals":          rule.Approved,
			"required_approvals": rule.MinApprovals,
			"ok":                 ok,
		}
		if len(rule.ApprovedMemberIDs) > 0 {
			payload["approved_member_ids"] = rule.ApprovedMemberIDs
		}
		ev := events.Event{
			Type:    "team_quorum_result",
			UserSub: userSub,
			TraceID: traceID,
			Payload: payload,
		}
		hub.PublishTo([]string{userSub}, ev)
	}
}

func publishTeamRuntimeMembersUpdated(
	hub *events.Hub,
	userSub,
	teamID,
	teamRunID string,
	runtimeMembers []map[string]any,
	traceID string,
) {
	if hub == nil || userSub == "" {
		return
	}
	payload := map[string]any{
		"team_id":         teamID,
		"team_run_id":     teamRunID,
		"runtime_members": runtimeMembers,
		"count":           len(runtimeMembers),
	}
	ev := events.Event{
		Type:    "team_runtime_members_updated",
		UserSub: userSub,
		TraceID: traceID,
		Payload: payload,
	}
	hub.PublishTo([]string{userSub}, ev)
}

func publishTeamRunCreated(
	hub *events.Hub,
	userSub,
	teamID,
	teamRunID,
	status,
	mode,
	createdBy string,
	createdUnixMs int64,
	summary map[string]any,
	traceID string,
) {
	if hub == nil || userSub == "" {
		return
	}
	payload := teamRunEventPayload(teamID, teamRunID, status, mode, createdUnixMs, summary)
	if createdBy != "" {
		payload["created_by"] = createdBy
	}
	ev := events.Event{
		Type:    "team_run_created",
		UserSub: userSub,
		TraceID: traceID,
		Payload: payload,
	}
	hub.PublishTo([]string{userSub}, ev)
}

func publishTeamRunStatus(
	hub *events.Hub,
	userSub,
	teamID,
	teamRunID,
	status,
	mode string,
	createdUnixMs int64,
	summary map[string]any,
	traceID string,
) {
	if hub == nil || userSub == "" {
		return
	}
	ev := events.Event{
		Type:    "team_run_status",
		UserSub: userSub,
		TraceID: traceID,
		Payload: teamRunEventPayload(teamID, teamRunID, status, mode, createdUnixMs, summary),
	}
	hub.PublishTo([]string{userSub}, ev)
}

func teamRunEventPayload(teamID, teamRunID, status, mode string, createdUnixMs int64, summary map[string]any) map[string]any {
	payload := map[string]any{
		"team_id":     teamID,
		"team_run_id": teamRunID,
		"status":      status,
	}
	if mode != "" {
		payload["mode"] = mode
	}
	if createdUnixMs > 0 {
		payload["created_unix_ms"] = createdUnixMs
	}
	if summary != nil {
		payload["member_job_summary"] = summary
	}
	return payload
}
