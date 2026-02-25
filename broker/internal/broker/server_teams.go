package broker

import (
	"context"
	"encoding/json"
	"fmt"
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
	t, err := s.cfg.DB.CreateTeam(r.Context(), p.Sub, teamID, displayName, req.Tags, req.PolicyRef, req.SharedMemoryScopeID, req.Meta)
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
			s.handleTeamRunCreate(w, r, teamID)
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
	_, ok := s.requireTeamOwner(w, r, p, teamID)
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

	options := parseTeamRunOptions(teamMeta)
	overrides, err := parseTeamRunOverrides(teamMeta)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusBadRequest)
		return
	}
	teamMeta["run_overrides_mode"] = overrides.Mode
	if overrides.Mode != "explicit" {
		delete(teamMeta, "member_overrides")
	} else if len(overrides.MemberOverrides) > 0 {
		teamMeta["member_overrides"] = overrides.MemberOverrides
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
	memberRunBodies := make([][]byte, 0, len(runMembers))
	for _, member := range runMembers {
		runForMember := map[string]any{}
		for k, v := range runMap {
			runForMember[k] = v
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

	teamRunID := "tr_" + newID()[:12]
	if _, err := s.cfg.DB.CreateTeamRun(r.Context(), teamRunID, teamID, "running", p.Sub, mustJSON(map[string]any{
		"run":  runMap,
		"team": teamMeta,
	})); err != nil {
		writeErrorJSON(w, "create team run failed", http.StatusBadRequest)
		return
	}
	if len(approvals) > 0 {
		if err := s.persistTeamRunApprovals(r.Context(), teamID, teamRunID, teamRunRules, approvals, membersByID, p.Sub); err != nil {
			_ = s.cfg.DB.UpdateTeamRunStatus(r.Context(), teamID, teamRunID, "failed")
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
	run, err := s.cfg.DB.GetTeamRun(r.Context(), teamID, teamRunID)
	if err != nil {
		writeErrorJSON(w, "team run not found", http.StatusNotFound)
		return
	}
	writeJSON(w, map[string]any{
		"ok":              true,
		"team_id":         run.TeamID,
		"team_run_id":     run.TeamRunID,
		"status":          run.Status,
		"created_unix_ms": run.CreatedAt.UnixMilli(),
	})
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
	var runtimeMembers any
	if len(run.RunJSON) > 0 {
		var runPayload map[string]any
		if err := json.Unmarshal(run.RunJSON, &runPayload); err == nil {
			if teamRaw, ok := runPayload["team"].(map[string]any); ok {
				if raw, ok := teamRaw["runtime_members"]; ok {
					runtimeMembers = raw
				}
			}
		}
	}
	members, err := s.cfg.DB.ListTeamMembers(r.Context(), teamID)
	if err != nil {
		writeErrorJSON(w, "db error", http.StatusInternalServerError)
		return
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
		"created_unix_ms": run.CreatedAt.UnixMilli(),
		"members":         outMembers,
	}
	if runtimeMembers != nil {
		resp["runtime_members"] = runtimeMembers
	}
	writeJSON(w, resp)
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

func parseTeamRunOptions(meta map[string]any) teamRunOptions {
	out := teamRunOptions{
		MaxConcurrency: 4,
		TimeoutMS:      60_000,
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
	return out
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
