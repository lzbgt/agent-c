package broker

import (
	"encoding/json"
	"net/http"
	"strings"

	"agentd-broker/internal/db"
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
	if !s.allowAutomationPrincipal(p) {
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
	if !s.allowAutomationPrincipal(p) {
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
	// - /v1/teams/{team_id}/runs/{team_run_id}/moderator/directive
	// - /v1/teams/{team_id}/runs/{team_run_id}/moderator/task
	// - /v1/teams/{team_id}/runs/{team_run_id}/moderator/events
	// - /v1/teams/{team_id}/runtime_members/allocate
	// - /v1/teams/{team_id}/guidance
	// - /v1/teams/{team_id}/guidance/{guidance_id}
	// - /v1/teams/{team_id}/guidance/{guidance_id}/ack
	// - /v1/teams/{team_id}/orchestrator/runs
	// - /v1/teams/{team_id}/orchestrator/runs/{orchestrator_run_id}
	// - /v1/teams/{team_id}/orchestrator/runs/{orchestrator_run_id}/heartbeat
	// - /v1/teams/{team_id}/orchestrator/spawn_requests
	// - /v1/teams/{team_id}/orchestrator/spawn_requests/{spawn_request_id}

	rest := strings.TrimPrefix(r.URL.Path, "/v1/teams/")
	parts := strings.SplitN(rest, "/", 5)
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
	case "guidance":
		if len(parts) == 2 || parts[2] == "" {
			switch r.Method {
			case "GET":
				s.handleTeamGuidanceList(w, r, teamID)
			case "POST":
				s.handleTeamGuidanceCreate(w, r, teamID)
			default:
				writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
			}
			return
		}
		if len(parts) >= 4 && parts[3] == "receipts" {
			s.handleTeamGuidanceReceipts(w, r, teamID, parts[2])
			return
		}
		if len(parts) >= 4 && parts[3] == "ack" {
			s.handleTeamGuidanceAck(w, r, teamID, parts[2])
			return
		}
		switch r.Method {
		case "GET":
			s.handleTeamGuidanceGet(w, r, teamID, parts[2])
		default:
			writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
		}
		return
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
	case "runtime_members":
		if len(parts) >= 3 && parts[2] == "allocate" {
			if r.Method == "POST" {
				s.handleTeamRuntimeMembersAllocate(w, r, teamID)
				return
			}
			writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		writeErrorJSON(w, "not found", http.StatusNotFound)
		return
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
			case "goal":
				if r.Method == "POST" {
					s.handleTeamRunGoalUpdate(w, r, teamID, parts[2])
					return
				}
				writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
				return
			case "handoff":
				if r.Method == "POST" {
					s.handleTeamRunHandoffUpdate(w, r, teamID, parts[2])
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
			case "moderator":
				if len(parts) < 5 || parts[4] == "" {
					writeErrorJSON(w, "not found", http.StatusNotFound)
					return
				}
				switch parts[4] {
				case "directive":
					s.handleTeamRunModeratorDirective(w, r, teamID, parts[2])
					return
				case "task":
					s.handleTeamRunModeratorTask(w, r, teamID, parts[2])
					return
				case "events":
					s.handleTeamRunModeratorEvents(w, r, teamID, parts[2])
					return
				default:
					writeErrorJSON(w, "not found", http.StatusNotFound)
					return
				}
			default:
				writeErrorJSON(w, "not found", http.StatusNotFound)
				return
			}
		}
		s.handleTeamRunGet(w, r, teamID, parts[2])
	case "orchestrator":
		if len(parts) < 3 || parts[2] == "" {
			writeErrorJSON(w, "not found", http.StatusNotFound)
			return
		}
		switch parts[2] {
		case "runs":
			if len(parts) == 3 || parts[3] == "" {
				switch r.Method {
				case "GET":
					s.handleTeamOrchestratorRunsList(w, r, teamID)
				case "POST":
					s.handleTeamOrchestratorRunCreate(w, r, teamID)
				default:
					writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
				}
				return
			}
			if len(parts) >= 5 && parts[4] == "heartbeat" {
				s.handleTeamOrchestratorRunHeartbeat(w, r, teamID, parts[3])
				return
			}
			switch r.Method {
			case "GET":
				s.handleTeamOrchestratorRunGet(w, r, teamID, parts[3])
			case "PATCH":
				s.handleTeamOrchestratorRunUpdate(w, r, teamID, parts[3])
			default:
				writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
			}
			return
		case "spawn_requests":
			if len(parts) == 3 || parts[3] == "" {
				switch r.Method {
				case "GET":
					s.handleTeamOrchestratorSpawnRequestsList(w, r, teamID)
				case "POST":
					s.handleTeamOrchestratorSpawnRequestCreate(w, r, teamID)
				default:
					writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
				}
				return
			}
			switch r.Method {
			case "GET":
				s.handleTeamOrchestratorSpawnRequestGet(w, r, teamID, parts[3])
			case "PATCH":
				s.handleTeamOrchestratorSpawnRequestUpdate(w, r, teamID, parts[3])
			default:
				writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
			}
			return
		default:
			writeErrorJSON(w, "not found", http.StatusNotFound)
			return
		}
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
	if !s.allowAutomationPrincipal(p) {
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
	if !s.allowAutomationPrincipal(p) {
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
	if !s.allowAutomationPrincipal(p) {
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
	if !s.allowAutomationPrincipal(p) {
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
	if !s.allowAutomationPrincipal(p) {
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

func (s *Server) handleTeamRuntimeMembersAllocate(w http.ResponseWriter, r *http.Request, teamID string) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if !s.allowAutomationPrincipal(p) {
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
		Roles                 []string `json:"roles"`
		ExistingRuntime       []any    `json:"existing_runtime_members"`
		ExcludeTeamMembers    *bool    `json:"exclude_team_members"`
		MaxMembers            *int     `json:"max_members"`
		PreferConnectedAgents *bool    `json:"prefer_connected"`
	}{}
	if len(body) > 0 {
		if err := json.Unmarshal(body, &req); err != nil {
			writeErrorJSON(w, "invalid json", http.StatusBadRequest)
			return
		}
	}
	roles := normalizeRoleList(req.Roles)
	if len(roles) == 0 {
		writeErrorJSON(w, "roles required", http.StatusBadRequest)
		return
	}
	excludeTeam := true
	if req.ExcludeTeamMembers != nil {
		excludeTeam = *req.ExcludeTeamMembers
	}
	maxMembers := 0
	if req.MaxMembers != nil && *req.MaxMembers > 0 {
		maxMembers = *req.MaxMembers
	}
	_ = req.PreferConnectedAgents

	existingRoles := map[string]bool{}
	usedAgentIDs := map[string]bool{}

	if excludeTeam {
		members, err := s.cfg.DB.ListTeamMembers(r.Context(), teamID)
		if err != nil {
			writeErrorJSON(w, "db error", http.StatusInternalServerError)
			return
		}
		for _, m := range members {
			role := strings.ToLower(strings.TrimSpace(m.Role))
			if role != "" {
				existingRoles[role] = true
			}
			agentID := strings.TrimSpace(m.AgentID)
			if agentID != "" {
				usedAgentIDs[agentID] = true
			}
		}
	}

	if len(req.ExistingRuntime) > 0 {
		inputs, err := parseTeamRunRuntimeMembers(map[string]any{"runtime_members": req.ExistingRuntime})
		if err != nil {
			writeErrorJSON(w, err.Error(), http.StatusBadRequest)
			return
		}
		for _, input := range inputs {
			role := strings.ToLower(strings.TrimSpace(input.Role))
			if role != "" {
				existingRoles[role] = true
			}
			agentID := strings.TrimSpace(input.AgentID)
			if agentID != "" {
				usedAgentIDs[agentID] = true
			}
		}
	}

	candidates, err := s.collectRuntimeAgentCandidates(r.Context(), p.Sub)
	if err != nil {
		msg := err.Error()
		status := http.StatusBadRequest
		if msg == "db error" || msg == "broker not initialized" {
			status = http.StatusInternalServerError
		}
		writeErrorJSON(w, msg, status)
		return
	}

	allocations, allocatedRoles, missingRoles, warning := allocateRuntimeMembersByRole(
		roles,
		candidates,
		existingRoles,
		usedAgentIDs,
		maxMembers,
	)

	runtimeMembers := make([]map[string]any, 0, len(allocations))
	for _, input := range allocations {
		entry := map[string]any{
			"agent_id": input.AgentID,
			"role":     input.Role,
		}
		if input.DeploymentID != "" {
			entry["deployment_id"] = input.DeploymentID
		}
		runtimeMembers = append(runtimeMembers, entry)
	}
	resp := map[string]any{
		"ok":              true,
		"team_id":         teamID,
		"runtime_members": runtimeMembers,
	}
	if len(allocatedRoles) > 0 {
		resp["allocated_roles"] = allocatedRoles
	}
	if len(missingRoles) > 0 {
		resp["missing_roles"] = missingRoles
	}
	if warning != "" {
		resp["warnings"] = []string{warning}
	}
	writeJSON(w, resp)
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
	if !s.allowAutomationPrincipal(p) {
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
	if !s.allowAutomationPrincipal(p) {
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
	if !s.allowAutomationPrincipal(p) {
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
	if !s.allowAutomationPrincipal(p) {
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
	if !s.allowAutomationPrincipal(p) {
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
	if !s.allowAutomationPrincipal(p) {
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
