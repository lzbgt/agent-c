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
	roleInstructionsProvided := false
	if _, ok := teamMeta["role_instructions"]; ok {
		roleInstructionsProvided = true
	}
	roleInstructions, err := parseRoleInstructions(teamMeta["role_instructions"])
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusBadRequest)
		return
	}
	rolePromptMode, err := parseRolePromptMode(teamMeta["role_prompt_mode"])
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusBadRequest)
		return
	}
	if !roleInstructionsProvided && team != nil {
		if teamMetaDefaults := team.Meta(); len(teamMetaDefaults) > 0 {
			if rawDefaults, ok := teamMetaDefaults["role_instructions"]; ok {
				defaults, err := parseRoleInstructions(rawDefaults)
				if err != nil {
					writeErrorJSON(w, "invalid team role_instructions", http.StatusBadRequest)
					return
				}
				if len(defaults) > 0 {
					roleInstructions = defaults
					teamMeta["role_instructions"] = defaults
				}
			}
			if rolePromptMode == "" {
				if rawMode, ok := teamMetaDefaults["role_prompt_mode"]; ok {
					mode, err := parseRolePromptMode(rawMode)
					if err != nil {
						writeErrorJSON(w, "invalid team role_prompt_mode", http.StatusBadRequest)
						return
					}
					if mode != "" {
						rolePromptMode = mode
					}
				}
			}
		}
	}
	if len(roleInstructions) > 0 {
		teamMeta["role_instructions"] = roleInstructions
	} else {
		delete(teamMeta, "role_instructions")
	}
	if rolePromptMode == "" && len(roleInstructions) > 0 {
		rolePromptMode = "prepend"
	}
	if rolePromptMode != "" {
		teamMeta["role_prompt_mode"] = rolePromptMode
	} else {
		delete(teamMeta, "role_prompt_mode")
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
	sharedMemoryMode, err := parseSharedMemoryMode(teamMeta["shared_memory_mode"])
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusBadRequest)
		return
	}
	if sharedMemoryMode == "" {
		if rawMode, ok := teamMeta["memory_scope_mode"]; ok {
			mode, err := parseSharedMemoryMode(rawMode)
			if err != nil {
				writeErrorJSON(w, strings.Replace(err.Error(), "shared_memory_mode", "memory_scope_mode", 1), http.StatusBadRequest)
				return
			}
			sharedMemoryMode = mode
		}
	}
	if sharedMemoryMode == "" && team != nil {
		if teamMetaDefaults := team.Meta(); len(teamMetaDefaults) > 0 {
			if rawMode, ok := teamMetaDefaults["shared_memory_mode"]; ok {
				mode, err := parseSharedMemoryMode(rawMode)
				if err != nil {
					writeErrorJSON(w, "invalid team shared_memory_mode", http.StatusBadRequest)
					return
				}
				sharedMemoryMode = mode
			}
		}
	}
	sharedMemoryScopeID := ""
	if v, ok := teamMeta["shared_memory_scope_id"]; ok {
		if s, ok := v.(string); ok {
			sharedMemoryScopeID = strings.TrimSpace(s)
		} else if v != nil {
			writeErrorJSON(w, "shared_memory_scope_id must be string", http.StatusBadRequest)
			return
		}
	}
	if sharedMemoryScopeID == "" && team != nil {
		sharedMemoryScopeID = strings.TrimSpace(team.SharedMemoryScopeID)
	}
	runMemoryScopeID := ""
	if v, ok := runMap["memory_scope_id"]; ok {
		if s, ok := v.(string); ok {
			runMemoryScopeID = strings.TrimSpace(s)
		} else if v != nil {
			writeErrorJSON(w, "memory_scope_id must be string", http.StatusBadRequest)
			return
		}
	}
	runMemoryScopeMode := ""
	if v, ok := runMap["memory_scope_mode"]; ok {
		mode, err := parseSharedMemoryMode(v)
		if err != nil {
			writeErrorJSON(w, strings.Replace(err.Error(), "shared_memory_mode", "memory_scope_mode", 1), http.StatusBadRequest)
			return
		}
		runMemoryScopeMode = mode
	}
	effectiveMemoryScopeID := runMemoryScopeID
	if effectiveMemoryScopeID == "" {
		effectiveMemoryScopeID = sharedMemoryScopeID
	}
	effectiveMemoryMode := runMemoryScopeMode
	if effectiveMemoryMode == "" {
		effectiveMemoryMode = sharedMemoryMode
	}
	if effectiveMemoryScopeID == "" && effectiveMemoryMode != "" {
		writeErrorJSON(w, "memory_scope_mode requires memory_scope_id", http.StatusBadRequest)
		return
	}
	if effectiveMemoryScopeID != "" && effectiveMemoryMode == "" {
		effectiveMemoryMode = "read_write"
	}
	if effectiveMemoryScopeID != "" {
		runMap["memory_scope_id"] = effectiveMemoryScopeID
		runMap["memory_scope_mode"] = effectiveMemoryMode
		teamMeta["shared_memory_scope_id"] = effectiveMemoryScopeID
		teamMeta["shared_memory_mode"] = effectiveMemoryMode
		delete(teamMeta, "memory_scope_mode")
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
	toolApprovalRules := []teamPolicyApprovalRule{}
	if quorumPolicyMode != "off" || len(approvals) > 0 {
		rules, err := s.cfg.DB.ListTeamQuorumRules(r.Context(), teamID)
		if err != nil {
			writeErrorJSON(w, "db error", http.StatusInternalServerError)
			return
		}
		teamRunRules = filterTeamRunRules(rules)
		toolApprovalRules = buildPolicyApprovalRules(rules)
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
	autoAllocateRoles := false
	if v, ok := teamMeta["auto_allocate_roles"]; ok {
		if b, ok := asBool(v); ok {
			autoAllocateRoles = b
		} else {
			writeErrorJSON(w, "auto_allocate_roles must be boolean", http.StatusBadRequest)
			return
		}
	}
	autoAllocateMaxMembers := 0
	if v, ok := teamMeta["auto_allocate_max_members"]; ok {
		if iv, ok := asInt(v); ok {
			if iv < 1 {
				iv = 1
			}
			if iv > 16 {
				iv = 16
			}
			autoAllocateMaxMembers = iv
		} else {
			writeErrorJSON(w, "auto_allocate_max_members must be integer", http.StatusBadRequest)
			return
		}
	}
	if autoAllocateRoles {
		teamMeta["auto_allocate_roles"] = true
	} else {
		delete(teamMeta, "auto_allocate_roles")
	}
	if autoAllocateMaxMembers > 0 {
		teamMeta["auto_allocate_max_members"] = autoAllocateMaxMembers
	} else {
		delete(teamMeta, "auto_allocate_max_members")
	}
	if autoAllocateRoles {
		rolePlan := []string{}
		if len(options.Roles) > 0 {
			for r := range options.Roles {
				if r != "" {
					rolePlan = append(rolePlan, r)
				}
			}
		} else if options.Role != "" {
			rolePlan = []string{options.Role}
		}
		if len(rolePlan) == 0 {
			rolePlan = collectRolePlanRoles(teamMeta, roleOverrides, roleInstructions)
		}
		if len(rolePlan) == 0 {
			teamMeta["auto_allocate_warning"] = "no role plan roles available"
		} else {
			existingRoles := map[string]bool{}
			usedAgentIDs := map[string]bool{}
			for _, member := range members {
				role := strings.ToLower(strings.TrimSpace(member.Role))
				if role != "" {
					existingRoles[role] = true
				}
				agentID := strings.TrimSpace(member.AgentID)
				if agentID != "" {
					usedAgentIDs[agentID] = true
				}
			}
			for _, input := range runtimeInputs {
				role := strings.ToLower(strings.TrimSpace(input.Role))
				if role != "" {
					existingRoles[role] = true
				}
				agentID := strings.TrimSpace(input.AgentID)
				if agentID != "" {
					usedAgentIDs[agentID] = true
				}
			}
			candidates, err := s.collectRuntimeAgentCandidates(r.Context(), p.Sub)
			if err != nil {
				teamMeta["auto_allocate_warning"] = err.Error()
			} else {
				allocations, allocatedRoles, missingRoles, warning := allocateRuntimeMembersByRole(
					rolePlan,
					candidates,
					existingRoles,
					usedAgentIDs,
					autoAllocateMaxMembers,
				)
				if len(allocations) > 0 {
					runtimeInputs = append(runtimeInputs, allocations...)
				}
				if len(allocatedRoles) > 0 {
					teamMeta["auto_allocate_allocated_roles"] = allocatedRoles
				}
				if len(missingRoles) > 0 {
					teamMeta["auto_allocate_missing_roles"] = missingRoles
				}
				if warning != "" {
					teamMeta["auto_allocate_warning"] = warning
				}
			}
		}
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

	teamRunID := "tr_" + newID()[:12]
	memberOverridesApplied := map[string]map[string]any{}
	roleOverridesApplied := map[string]map[string]any{}
	memberRunBodies := make([][]byte, 0, len(runMembers))
	memberSessions := map[string]string{}
	policyRulesPayload := []any{}
	if len(toolApprovalRules) > 0 {
		policyRulesPayload = make([]any, 0, len(toolApprovalRules))
		for _, rule := range toolApprovalRules {
			policyRulesPayload = append(policyRulesPayload, rule)
		}
	}
	noSession := false
	if v, ok := runMap["no_session"]; ok {
		if b, ok := v.(bool); ok && b {
			noSession = true
		}
	}
	var runSessionID string
	if raw, ok := runMap["session_id"]; ok {
		s, ok := raw.(string)
		if !ok {
			writeErrorJSON(w, "session_id must be string", http.StatusBadRequest)
			return
		}
		runSessionID = strings.TrimSpace(s)
		if runSessionID != "" && !isSessionIDSafe(runSessionID) {
			writeErrorJSON(w, "invalid session_id", http.StatusBadRequest)
			return
		}
	}
	if noSession && runSessionID != "" {
		writeErrorJSON(w, "no_session true with session_id set", http.StatusBadRequest)
		return
	}
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
		if roleKey != "" && len(roleInstructions) > 0 {
			if instr, ok := roleInstructions[roleKey]; ok && strings.TrimSpace(instr) != "" {
				basePrompt := ""
				if rawPrompt, ok := runForMember["prompt"]; ok {
					if s, ok := rawPrompt.(string); ok {
						basePrompt = s
					}
				}
				finalPrompt := applyRoleInstruction(instr, basePrompt, rolePromptMode)
				if finalPrompt != "" {
					runForMember["prompt"] = finalPrompt
				}
			}
		}
		if len(policyRulesPayload) > 0 {
			merged := make([]any, 0, len(policyRulesPayload))
			if existing, ok := runForMember["policy_approval_rules"]; ok {
				if items, ok := existing.([]any); ok {
					merged = append(merged, items...)
				}
			}
			merged = append(merged, policyRulesPayload...)
			runForMember["policy_approval_rules"] = merged
			runForMember["policy_mode"] = "enforce"
		}
		runForMember["team_id"] = teamID
		if !noSession {
			sessionID := runSessionID
			if sessionID == "" {
				sessionID = makeTeamRunSessionID(teamID, teamRunID, member.MemberID)
			}
			if sessionID != "" {
				runForMember["session_id"] = sessionID
				memberSessions[member.MemberID] = sessionID
			}
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

	if len(memberSessions) > 0 {
		teamMeta["member_sessions"] = memberSessions
	}
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
	if !s.allowAutomationPrincipal(p) {
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
	if !s.allowAutomationPrincipal(p) {
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
	if !s.allowAutomationPrincipal(p) {
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
	if !s.allowAutomationPrincipal(p) {
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

	runMap, _ := runPayload["run"].(map[string]any)
	noSession := false
	var runSessionID string
	if runMap != nil {
		if v, ok := runMap["no_session"]; ok {
			if b, ok := v.(bool); ok && b {
				noSession = true
			}
		}
		if raw, ok := runMap["session_id"]; ok {
			if s, ok := raw.(string); ok {
				runSessionID = strings.TrimSpace(s)
				if runSessionID != "" && !isSessionIDSafe(runSessionID) {
					runSessionID = ""
				}
			}
		}
	}
	if !noSession {
		memberSessions := teamRunMemberSessionsFromMeta(teamMeta)
		if runSessionID != "" || memberSessions != nil {
			if memberSessions == nil {
				memberSessions = map[string]string{}
			}
			for _, m := range runtimeMembers {
				if m.MemberID == "" {
					continue
				}
				if _, ok := memberSessions[m.MemberID]; ok {
					continue
				}
				if runSessionID != "" {
					memberSessions[m.MemberID] = runSessionID
				} else {
					memberSessions[m.MemberID] = makeTeamRunSessionID(teamID, run.TeamRunID, m.MemberID)
				}
			}
			if len(memberSessions) > 0 {
				teamMeta["member_sessions"] = memberSessions
			}
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

func (s *Server) handleTeamRunGoalUpdate(w http.ResponseWriter, r *http.Request, teamID, teamRunID string) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if !s.allowAutomationPrincipal(p) {
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
	raw := map[string]any{}
	if err := json.Unmarshal(body, &raw); err != nil {
		writeErrorJSON(w, "invalid json", http.StatusBadRequest)
		return
	}
	rawContract := raw["goal_contract"]
	rawEvent := raw["event"]
	if rawContract == nil && rawEvent == nil {
		writeErrorJSON(w, "goal_contract or event required", http.StatusBadRequest)
		return
	}
	contract, err := parseGoalContract(rawContract)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusBadRequest)
		return
	}
	var goalEvent teamGoalEventInput
	if rawEvent != nil {
		goalEvent, err = parseGoalEvent(rawEvent)
		if err != nil {
			writeErrorJSON(w, err.Error(), http.StatusBadRequest)
			return
		}
	}

	runPayload := map[string]any{}
	teamMeta := map[string]any{}
	if len(run.RunJSON) > 0 {
		if err := json.Unmarshal(run.RunJSON, &runPayload); err != nil {
			writeErrorJSON(w, "invalid stored run payload", http.StatusInternalServerError)
			return
		}
		if teamRaw, ok := runPayload["team"].(map[string]any); ok {
			teamMeta = teamRaw
		}
	}
	if teamMeta == nil {
		teamMeta = map[string]any{}
	}
	nowMs := time.Now().UTC().UnixMilli()
	if len(contract) > 0 {
		teamMeta["goal_contract"] = contract
		teamMeta["goal_updated_unix_ms"] = nowMs
	}
	var goalEvents []map[string]any
	if rawEvent != nil {
		goalEvents, err = appendGoalEvent(teamMeta, goalEvent, maxGoalEvents)
		if err != nil {
			writeErrorJSON(w, err.Error(), http.StatusBadRequest)
			return
		}
		teamMeta["goal_events_updated_unix_ms"] = nowMs
	}
	runPayload["team"] = teamMeta
	if err := s.cfg.DB.UpdateTeamRunPayload(r.Context(), teamID, teamRunID, mustJSON(runPayload)); err != nil {
		writeErrorJSON(w, "update team run failed", http.StatusInternalServerError)
		return
	}
	traceID := traceIDFromContext(r.Context())
	if rawEvent != nil && len(goalEvents) > 0 {
		lastEvent := goalEvents[len(goalEvents)-1]
		payload := map[string]any{
			"team_id":     teamID,
			"team_run_id": teamRunID,
			"event":       lastEvent,
		}
		switch goalEvent.Type {
		case "progress":
			publishTeamGoalEvent(s.cfg.Events, p.Sub, "team_goal_progress", payload, traceID)
		case "drift":
			publishTeamGoalEvent(s.cfg.Events, p.Sub, "team_goal_drift", payload, traceID)
		case "spawn_validation":
			publishTeamGoalEvent(s.cfg.Events, p.Sub, "team_goal_spawn_validation", payload, traceID)
		}
	}
	resp := map[string]any{
		"ok":          true,
		"team_id":     teamID,
		"team_run_id": teamRunID,
	}
	if len(contract) > 0 {
		resp["goal_contract"] = contract
	}
	if goalEvents != nil {
		resp["goal_events"] = goalEvents
		resp["goal_event_count"] = len(goalEvents)
	}
	writeJSON(w, resp)
}

func (s *Server) handleTeamRunHandoffUpdate(w http.ResponseWriter, r *http.Request, teamID, teamRunID string) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if !s.allowAutomationPrincipal(p) {
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
	raw := map[string]any{}
	if err := json.Unmarshal(body, &raw); err != nil {
		writeErrorJSON(w, "invalid json", http.StatusBadRequest)
		return
	}
	rawEvent := raw["event"]
	if rawEvent == nil {
		writeErrorJSON(w, "event required", http.StatusBadRequest)
		return
	}
	handoffEvent, err := parseHandoffEvent(rawEvent)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusBadRequest)
		return
	}

	runPayload := map[string]any{}
	teamMeta := map[string]any{}
	if len(run.RunJSON) > 0 {
		if err := json.Unmarshal(run.RunJSON, &runPayload); err != nil {
			writeErrorJSON(w, "invalid stored run payload", http.StatusInternalServerError)
			return
		}
		if teamRaw, ok := runPayload["team"].(map[string]any); ok {
			teamMeta = teamRaw
		}
	}
	if teamMeta == nil {
		teamMeta = map[string]any{}
	}
	handoffEvents, err := appendHandoffEvent(teamMeta, handoffEvent, maxHandoffEvents)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusBadRequest)
		return
	}
	teamMeta["handoff_events_updated_unix_ms"] = time.Now().UTC().UnixMilli()
	runPayload["team"] = teamMeta
	if err := s.cfg.DB.UpdateTeamRunPayload(r.Context(), teamID, teamRunID, mustJSON(runPayload)); err != nil {
		writeErrorJSON(w, "update team run failed", http.StatusInternalServerError)
		return
	}
	traceID := traceIDFromContext(r.Context())
	if len(handoffEvents) > 0 {
		lastEvent := handoffEvents[len(handoffEvents)-1]
		payload := map[string]any{
			"team_id":     teamID,
			"team_run_id": teamRunID,
			"event":       lastEvent,
		}
		publishTeamHandoffEvent(s.cfg.Events, p.Sub, payload, traceID)
	}
	resp := map[string]any{
		"ok":                  true,
		"team_id":             teamID,
		"team_run_id":         teamRunID,
		"handoff_events":      handoffEvents,
		"handoff_event_count": len(handoffEvents),
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
	var roleGraph any
	if raw, ok := teamMeta["role_graph"]; ok {
		roleGraph = raw
	}
	var roleInstructions any
	if raw, ok := teamMeta["role_instructions"]; ok {
		roleInstructions = raw
	}
	var rolePromptMode any
	if raw, ok := teamMeta["role_prompt_mode"]; ok {
		rolePromptMode = raw
	}
	var runOverridesMode any
	if raw, ok := teamMeta["run_overrides_mode"]; ok {
		runOverridesMode = raw
	}
	var autoAllocateRoles any
	if raw, ok := teamMeta["auto_allocate_roles"]; ok {
		autoAllocateRoles = raw
	}
	var autoAllocateAllocated any
	if raw, ok := teamMeta["auto_allocate_allocated_roles"]; ok {
		autoAllocateAllocated = raw
	}
	var autoAllocateMissing any
	if raw, ok := teamMeta["auto_allocate_missing_roles"]; ok {
		autoAllocateMissing = raw
	}
	var autoAllocateWarning any
	if raw, ok := teamMeta["auto_allocate_warning"]; ok {
		autoAllocateWarning = raw
	}
	var sharedMemoryScope any
	if raw, ok := teamMeta["shared_memory_scope_id"]; ok {
		sharedMemoryScope = raw
	}
	var sharedMemoryMode any
	if raw, ok := teamMeta["shared_memory_mode"]; ok {
		sharedMemoryMode = raw
	}
	var memberJobs any
	if raw, ok := teamMeta["member_jobs"]; ok {
		memberJobs = raw
	}
	var memberSessions any
	if raw, ok := teamMeta["member_sessions"]; ok {
		memberSessions = raw
	}
	var dispatchErrors any
	if raw, ok := teamMeta["dispatch_errors"]; ok {
		dispatchErrors = raw
	}
	var goalContract any
	if raw, ok := teamMeta["goal_contract"]; ok {
		goalContract = raw
	}
	var goalEvents any
	if raw, ok := teamMeta["goal_events"]; ok {
		goalEvents = raw
	}
	var handoffEvents any
	if raw, ok := teamMeta["handoff_events"]; ok {
		handoffEvents = raw
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
	if roleGraph != nil {
		resp["role_graph"] = roleGraph
	}
	if roleInstructions != nil {
		resp["role_instructions"] = roleInstructions
	}
	if rolePromptMode != nil {
		resp["role_prompt_mode"] = rolePromptMode
	}
	if runOverridesMode != nil {
		resp["run_overrides_mode"] = runOverridesMode
	}
	if autoAllocateRoles != nil {
		resp["auto_allocate_roles"] = autoAllocateRoles
	}
	if autoAllocateAllocated != nil {
		resp["auto_allocate_allocated_roles"] = autoAllocateAllocated
	}
	if autoAllocateMissing != nil {
		resp["auto_allocate_missing_roles"] = autoAllocateMissing
	}
	if autoAllocateWarning != nil {
		resp["auto_allocate_warning"] = autoAllocateWarning
	}
	if sharedMemoryScope != nil {
		resp["shared_memory_scope_id"] = sharedMemoryScope
	}
	if sharedMemoryMode != nil {
		resp["shared_memory_mode"] = sharedMemoryMode
	}
	if memberJobs != nil {
		resp["member_jobs"] = memberJobs
	}
	if memberSessions != nil {
		resp["member_sessions"] = memberSessions
	}
	if dispatchErrors != nil {
		resp["dispatch_errors"] = dispatchErrors
	}
	if goalContract != nil {
		resp["goal_contract"] = goalContract
	}
	if goalEvents != nil {
		resp["goal_events"] = goalEvents
	}
	if handoffEvents != nil {
		resp["handoff_events"] = handoffEvents
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
	if !s.allowAutomationPrincipal(p) {
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
	if !s.allowAutomationPrincipal(p) {
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

func publishTeamGoalEvent(
	hub *events.Hub,
	userSub string,
	eventType string,
	payload map[string]any,
	traceID string,
) {
	if hub == nil || userSub == "" || eventType == "" {
		return
	}
	ev := events.Event{
		Type:    eventType,
		UserSub: userSub,
		TraceID: traceID,
		Payload: payload,
	}
	hub.PublishTo([]string{userSub}, ev)
}

func publishTeamHandoffEvent(
	hub *events.Hub,
	userSub string,
	payload map[string]any,
	traceID string,
) {
	if hub == nil || userSub == "" {
		return
	}
	ev := events.Event{
		Type:    "team_handoff",
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

func (s *Server) collectRuntimeAgentCandidates(ctx context.Context, ownerSub string) ([]runtimeAgentCandidate, error) {
	if s == nil || s.cfg.DB == nil || s.cfg.Registry == nil {
		return nil, errors.New("broker not initialized")
	}
	dbAgents, err := s.cfg.DB.ListAgentsForUser(ctx, ownerSub)
	if err != nil {
		return nil, errors.New("db error")
	}
	candidates := make([]runtimeAgentCandidate, 0, len(dbAgents))
	for _, a := range dbAgents {
		if !a.Enabled {
			continue
		}
		conns := s.cfg.Registry.ListByAgent(a.AgentID)
		if len(conns) == 0 {
			continue
		}
		var bestConnected time.Time
		bestDeployment := ""
		for _, conn := range conns {
			if conn == nil {
				continue
			}
			if bestDeployment == "" || conn.Connected.After(bestConnected) {
				bestConnected = conn.Connected
				bestDeployment = conn.DeploymentID
			}
		}
		if bestDeployment != "" {
			candidates = append(candidates, runtimeAgentCandidate{
				AgentID:      a.AgentID,
				DeploymentID: bestDeployment,
			})
		}
	}
	if len(candidates) == 0 {
		return nil, errors.New("no connected agents available")
	}
	return candidates, nil
}
