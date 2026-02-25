package broker

import (
	"encoding/json"
	"errors"
	"net/http"
	"net/url"
	"sort"
	"strings"
)

type teamRunModeratorTargets struct {
	Roles     []string `json:"roles"`
	MemberIDs []string `json:"member_ids"`
	AgentIDs  []string `json:"agent_ids"`
}

type teamRunModeratorBaseRequest struct {
	Assignees       []string                 `json:"assignees"`
	Priority        *int                     `json:"priority"`
	Metadata        map[string]any           `json:"metadata"`
	AppendToSession *bool                    `json:"append_to_session"`
	Actor           map[string]any           `json:"actor"`
	Targets         *teamRunModeratorTargets `json:"targets"`
	MaxConcurrency  *int                     `json:"max_concurrency"`
	TimeoutMS       *int                     `json:"timeout_ms"`
}

type teamRunModeratorDirectiveRequest struct {
	teamRunModeratorBaseRequest
	Directive string `json:"directive"`
	Scope     string `json:"scope"`
}

type teamRunModeratorTaskRequest struct {
	teamRunModeratorBaseRequest
	Title  string   `json:"title"`
	Detail string   `json:"detail"`
	Tags   []string `json:"tags"`
	Status string   `json:"status"`
}

type teamRunMemberInfo struct {
	MemberID     string
	AgentID      string
	DeploymentID string
	Role         string
}

type teamRunModeratorEventRow struct {
	MemberID     string
	AgentID      string
	DeploymentID string
	SessionID    string
	Type         string
	TsUnixMS     int64
	Event        map[string]any
}

type teamRunModeratorEventSource struct {
	MemberID     string
	AgentID      string
	DeploymentID string
	SessionID    string
}

func (s *Server) handleTeamRunModeratorDirective(w http.ResponseWriter, r *http.Request, teamID, teamRunID string) {
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
	if _, ok := s.requireTeamOwner(w, r, p, teamID); !ok {
		return
	}
	body, err := readBodyBounded(r.Body, 1024*1024)
	if err != nil {
		writeErrorJSON(w, "invalid body", http.StatusBadRequest)
		return
	}
	req := teamRunModeratorDirectiveRequest{}
	if len(body) > 0 {
		if err := json.Unmarshal(body, &req); err != nil {
			writeErrorJSON(w, "invalid json", http.StatusBadRequest)
			return
		}
	}
	directive := strings.TrimSpace(req.Directive)
	if directive == "" {
		writeErrorJSON(w, "missing directive", http.StatusBadRequest)
		return
	}
	scope := strings.TrimSpace(req.Scope)

	dispatch, skipped, err := s.dispatchTeamRunModerator(
		r,
		p,
		teamID,
		teamRunID,
		req.Targets,
		func(sessionID string) map[string]any {
			payload := map[string]any{
				"session_id": sessionID,
				"directive":  directive,
			}
			if scope != "" {
				payload["scope"] = scope
			}
			if len(req.Assignees) > 0 {
				payload["assignees"] = req.Assignees
			}
			if req.Priority != nil {
				payload["priority"] = *req.Priority
			}
			if req.Metadata != nil {
				payload["metadata"] = req.Metadata
			}
			if req.AppendToSession != nil {
				payload["append_to_session"] = *req.AppendToSession
			}
			if actor := normalizeModeratorActor(req.Actor, p); actor != nil {
				payload["actor"] = actor
			}
			return payload
		},
		req.MaxConcurrency,
		req.TimeoutMS,
	)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusBadRequest)
		return
	}
	resp := map[string]any{
		"ok":          true,
		"team_id":     teamID,
		"team_run_id": teamRunID,
		"dispatched":  dispatch,
	}
	if len(skipped) > 0 {
		resp["skipped"] = skipped
	}
	writeJSON(w, resp)
}

func (s *Server) handleTeamRunModeratorTask(w http.ResponseWriter, r *http.Request, teamID, teamRunID string) {
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
	if _, ok := s.requireTeamOwner(w, r, p, teamID); !ok {
		return
	}
	body, err := readBodyBounded(r.Body, 1024*1024)
	if err != nil {
		writeErrorJSON(w, "invalid body", http.StatusBadRequest)
		return
	}
	req := teamRunModeratorTaskRequest{}
	if len(body) > 0 {
		if err := json.Unmarshal(body, &req); err != nil {
			writeErrorJSON(w, "invalid json", http.StatusBadRequest)
			return
		}
	}
	title := strings.TrimSpace(req.Title)
	if title == "" {
		writeErrorJSON(w, "missing title", http.StatusBadRequest)
		return
	}
	detail := strings.TrimSpace(req.Detail)
	status := strings.TrimSpace(req.Status)

	dispatch, skipped, err := s.dispatchTeamRunModerator(
		r,
		p,
		teamID,
		teamRunID,
		req.Targets,
		func(sessionID string) map[string]any {
			payload := map[string]any{
				"session_id": sessionID,
				"title":      title,
			}
			if detail != "" {
				payload["detail"] = detail
			}
			if len(req.Tags) > 0 {
				payload["tags"] = req.Tags
			}
			if status != "" {
				payload["status"] = status
			}
			if len(req.Assignees) > 0 {
				payload["assignees"] = req.Assignees
			}
			if req.Priority != nil {
				payload["priority"] = *req.Priority
			}
			if req.Metadata != nil {
				payload["metadata"] = req.Metadata
			}
			if req.AppendToSession != nil {
				payload["append_to_session"] = *req.AppendToSession
			}
			if actor := normalizeModeratorActor(req.Actor, p); actor != nil {
				payload["actor"] = actor
			}
			return payload
		},
		req.MaxConcurrency,
		req.TimeoutMS,
	)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusBadRequest)
		return
	}
	resp := map[string]any{
		"ok":          true,
		"team_id":     teamID,
		"team_run_id": teamRunID,
		"dispatched":  dispatch,
	}
	if len(skipped) > 0 {
		resp["skipped"] = skipped
	}
	writeJSON(w, resp)
}

func (s *Server) handleTeamRunModeratorEvents(w http.ResponseWriter, r *http.Request, teamID, teamRunID string) {
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
	if _, ok := s.requireTeamOwner(w, r, p, teamID); !ok {
		return
	}

	run, err := s.cfg.DB.GetTeamRun(r.Context(), teamID, teamRunID)
	if err != nil {
		writeErrorJSON(w, "team run not found", http.StatusNotFound)
		return
	}

	query := r.URL.Query()
	types := strings.TrimSpace(query.Get("types"))
	maxBytes := 1_048_576
	if v := strings.TrimSpace(query.Get("max_bytes")); v != "" {
		if n, ok := parseIntBounded(v, 1, 5_000_000); ok {
			maxBytes = n
		}
	}
	limit := 200
	if v := strings.TrimSpace(query.Get("limit")); v != "" {
		if n, ok := parseIntBounded(v, 1, 5_000); ok {
			limit = n
		}
	}
	maxConcurrency := 4
	if v := strings.TrimSpace(query.Get("max_concurrency")); v != "" {
		if n, ok := parseIntBounded(v, 1, 16); ok {
			maxConcurrency = n
		}
	}
	timeoutMS := 10_000
	if v := strings.TrimSpace(query.Get("timeout_ms")); v != "" {
		if n, ok := parseIntBounded(v, 1, 120_000); ok {
			timeoutMS = n
		}
	}

	targets := parseTeamRunModeratorTargets(query)

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
	runMap, _ := runPayload["run"].(map[string]any)

	noSession := false
	var baseSessionID string
	if runMap != nil {
		if v, ok := runMap["no_session"]; ok {
			if b, ok := v.(bool); ok && b {
				noSession = true
			}
		}
		if raw, ok := runMap["session_id"]; ok {
			if s, ok := raw.(string); ok {
				s = strings.TrimSpace(s)
				if s != "" && isSessionIDSafe(s) {
					baseSessionID = s
				}
			}
		}
	}
	memberSessions := teamRunMemberSessionsFromMeta(teamMeta)

	members, err := s.cfg.DB.ListTeamMembers(r.Context(), teamID)
	if err != nil {
		writeErrorJSON(w, "db error", http.StatusInternalServerError)
		return
	}
	combined := make([]teamRunMemberInfo, 0, len(members))
	for _, m := range members {
		combined = append(combined, teamRunMemberInfo{
			MemberID:     m.MemberID,
			AgentID:      m.AgentID,
			DeploymentID: m.DeploymentID,
			Role:         m.Role,
		})
	}
	if rawMembers, ok := teamMeta["runtime_members"]; ok && rawMembers != nil {
		if arr, ok := rawMembers.([]any); ok {
			inputs, err := parseTeamRunRuntimeMembers(map[string]any{"runtime_members": arr})
			if err != nil {
				writeErrorJSON(w, err.Error(), http.StatusBadRequest)
				return
			}
			for _, input := range inputs {
				combined = append(combined, teamRunMemberInfo{
					MemberID:     strings.TrimSpace(input.MemberID),
					AgentID:      strings.TrimSpace(input.AgentID),
					DeploymentID: strings.TrimSpace(input.DeploymentID),
					Role:         strings.TrimSpace(input.Role),
				})
			}
		}
	}

	selected, skipped := filterTeamRunModeratorTargets(combined, targets)
	if len(selected) == 0 {
		writeJSON(w, map[string]any{
			"ok":          true,
			"team_id":     teamID,
			"team_run_id": teamRunID,
			"events":      []any{},
			"skipped":     skipped,
		})
		return
	}

	if noSession {
		writeJSON(w, map[string]any{
			"ok":          true,
			"team_id":     teamID,
			"team_run_id": teamRunID,
			"events":      []any{},
			"skipped":     append(skipped, map[string]any{"reason": "no_session"}),
		})
		return
	}

	for _, member := range selected {
		if strings.TrimSpace(member.AgentID) == "" {
			writeErrorJSON(w, "team member missing agent_id", http.StatusBadRequest)
			return
		}
		if ok, err := s.canAccessAgent(r.Context(), p, member.AgentID); err != nil {
			writeErrorJSON(w, "db error", http.StatusInternalServerError)
			return
		} else if !ok {
			writeErrorJSON(w, "forbidden", http.StatusForbidden)
			return
		}
	}

	tasks := make([]agentTaskPrepared, 0, len(selected))
	sources := make([]teamRunModeratorEventSource, 0, len(selected))
	skippedRows := skipped

	for idx, member := range selected {
		sessionID := ""
		if memberSessions != nil {
			sessionID = memberSessions[member.MemberID]
		}
		if sessionID == "" && baseSessionID != "" {
			sessionID = baseSessionID
		}
		if sessionID == "" {
			skippedRows = append(skippedRows, map[string]any{
				"member_id": member.MemberID,
				"agent_id":  member.AgentID,
				"reason":    "missing session_id",
			})
			continue
		}
		q := url.Values{}
		q.Set("session_id", sessionID)
		if types != "" {
			q.Set("types", types)
		}
		if maxBytes > 0 {
			q.Set("max_bytes", itoa(maxBytes))
		}
		tasks = append(tasks, agentTaskPrepared{
			TaskID:       "member_" + itoa(idx),
			AgentID:      member.AgentID,
			DeploymentID: member.DeploymentID,
			Method:       "GET",
			Path:         "/api/v1/moderator/events",
			Query:        q.Encode(),
			Headers:      map[string]string{},
		})
		sources = append(sources, teamRunModeratorEventSource{
			MemberID:     member.MemberID,
			AgentID:      member.AgentID,
			DeploymentID: member.DeploymentID,
			SessionID:    sessionID,
		})
	}

	if len(tasks) == 0 {
		writeJSON(w, map[string]any{
			"ok":          true,
			"team_id":     teamID,
			"team_run_id": teamRunID,
			"events":      []any{},
			"skipped":     skippedRows,
		})
		return
	}

	results := s.executeAgentTasks(r.Context(), p, tasks, maxConcurrency, timeoutMS, traceIDFromContext(r.Context()))
	events := make([]teamRunModeratorEventRow, 0, len(results))
	errorsOut := make([]map[string]any, 0)

	for idx, res := range results {
		if idx >= len(sources) {
			continue
		}
		src := sources[idx]
		if !res.OK {
			row := map[string]any{
				"member_id": src.MemberID,
				"agent_id":  src.AgentID,
			}
			if src.DeploymentID != "" {
				row["deployment_id"] = src.DeploymentID
			}
			if res.HTTPStatus != 0 {
				row["http_status"] = res.HTTPStatus
			}
			if res.Error != "" {
				row["error"] = res.Error
			}
			errorsOut = append(errorsOut, row)
			continue
		}
		rawEvents, ok := res.Result["events"].([]any)
		if !ok {
			continue
		}
		for _, item := range rawEvents {
			evMap, ok := item.(map[string]any)
			if !ok {
				continue
			}
			evType, _ := evMap["type"].(string)
			ts := int64(0)
			if v, ok := evMap["ts_unix_ms"]; ok {
				if n, ok := asInt(v); ok {
					ts = int64(n)
				}
			}
			events = append(events, teamRunModeratorEventRow{
				MemberID:     src.MemberID,
				AgentID:      src.AgentID,
				DeploymentID: src.DeploymentID,
				SessionID:    src.SessionID,
				Type:         evType,
				TsUnixMS:     ts,
				Event:        evMap,
			})
		}
	}

	sort.Slice(events, func(i, j int) bool {
		if events[i].TsUnixMS == events[j].TsUnixMS {
			return events[i].MemberID < events[j].MemberID
		}
		return events[i].TsUnixMS > events[j].TsUnixMS
	})
	if limit > 0 && len(events) > limit {
		events = events[:limit]
	}

	outEvents := make([]map[string]any, 0, len(events))
	for _, ev := range events {
		row := map[string]any{
			"member_id":  ev.MemberID,
			"agent_id":   ev.AgentID,
			"session_id": ev.SessionID,
			"type":       ev.Type,
			"ts_unix_ms": ev.TsUnixMS,
			"event":      ev.Event,
		}
		if ev.DeploymentID != "" {
			row["deployment_id"] = ev.DeploymentID
		}
		outEvents = append(outEvents, row)
	}

	resp := map[string]any{
		"ok":          true,
		"team_id":     teamID,
		"team_run_id": teamRunID,
		"events":      outEvents,
	}
	if len(errorsOut) > 0 {
		resp["errors"] = errorsOut
	}
	if len(skippedRows) > 0 {
		resp["skipped"] = skippedRows
	}
	writeJSON(w, resp)
}

func normalizeModeratorActor(raw map[string]any, p *Principal) map[string]any {
	if raw != nil && len(raw) > 0 {
		return raw
	}
	if p == nil {
		return nil
	}
	return map[string]any{
		"role": "moderator",
		"id":   p.Sub,
		"kind": "broker",
	}
}

func (s *Server) dispatchTeamRunModerator(
	r *http.Request,
	p *Principal,
	teamID string,
	teamRunID string,
	targets *teamRunModeratorTargets,
	buildPayload func(sessionID string) map[string]any,
	maxConcurrency *int,
	timeoutMS *int,
) ([]map[string]any, []map[string]any, error) {
	if p == nil {
		return nil, nil, errors.New("missing principal")
	}
	run, err := s.cfg.DB.GetTeamRun(r.Context(), teamID, teamRunID)
	if err != nil {
		return nil, nil, errors.New("team run not found")
	}

	var runPayload map[string]any
	if len(run.RunJSON) > 0 {
		if err := json.Unmarshal(run.RunJSON, &runPayload); err != nil {
			return nil, nil, errors.New("invalid stored run payload")
		}
	}
	if runPayload == nil {
		runPayload = map[string]any{}
	}
	teamMeta, _ := runPayload["team"].(map[string]any)
	if teamMeta == nil {
		teamMeta = map[string]any{}
	}
	runMap, _ := runPayload["run"].(map[string]any)

	noSession := false
	var baseSessionID string
	if runMap != nil {
		if v, ok := runMap["no_session"]; ok {
			if b, ok := v.(bool); ok && b {
				noSession = true
			}
		}
		if raw, ok := runMap["session_id"]; ok {
			if s, ok := raw.(string); ok {
				s = strings.TrimSpace(s)
				if s != "" && isSessionIDSafe(s) {
					baseSessionID = s
				}
			}
		}
	}
	memberSessions := teamRunMemberSessionsFromMeta(teamMeta)

	members, err := s.cfg.DB.ListTeamMembers(r.Context(), teamID)
	if err != nil {
		return nil, nil, errors.New("db error")
	}
	combined := make([]teamRunMemberInfo, 0, len(members))
	for _, m := range members {
		combined = append(combined, teamRunMemberInfo{
			MemberID:     m.MemberID,
			AgentID:      m.AgentID,
			DeploymentID: m.DeploymentID,
			Role:         m.Role,
		})
	}
	if rawMembers, ok := teamMeta["runtime_members"]; ok && rawMembers != nil {
		if arr, ok := rawMembers.([]any); ok {
			inputs, err := parseTeamRunRuntimeMembers(map[string]any{"runtime_members": arr})
			if err != nil {
				return nil, nil, err
			}
			for _, input := range inputs {
				combined = append(combined, teamRunMemberInfo{
					MemberID:     strings.TrimSpace(input.MemberID),
					AgentID:      strings.TrimSpace(input.AgentID),
					DeploymentID: strings.TrimSpace(input.DeploymentID),
					Role:         strings.TrimSpace(input.Role),
				})
			}
		}
	}
	selected, skipped := filterTeamRunModeratorTargets(combined, targets)
	if len(selected) == 0 {
		return nil, skipped, errors.New("no eligible members")
	}
	for _, member := range selected {
		if strings.TrimSpace(member.AgentID) == "" {
			return nil, skipped, errors.New("team member missing agent_id")
		}
		if ok, err := s.canAccessAgent(r.Context(), p, member.AgentID); err != nil {
			return nil, skipped, errors.New("db error")
		} else if !ok {
			return nil, skipped, errors.New("forbidden")
		}
	}

	tasks := make([]agentTaskPrepared, 0, len(selected))
	dispatchRows := make([]map[string]any, 0, len(selected))
	skippedRows := skipped

	for idx, member := range selected {
		sessionID := ""
		if !noSession {
			if memberSessions != nil {
				sessionID = memberSessions[member.MemberID]
			}
			if sessionID == "" && baseSessionID != "" {
				sessionID = baseSessionID
			}
		}
		if sessionID == "" {
			skippedRows = append(skippedRows, map[string]any{
				"member_id": member.MemberID,
				"agent_id":  member.AgentID,
				"reason":    "missing session_id",
			})
			continue
		}
		payload := buildPayload(sessionID)
		tasks = append(tasks, agentTaskPrepared{
			TaskID:       "member_" + itoa(idx),
			AgentID:      member.AgentID,
			DeploymentID: member.DeploymentID,
			Method:       "POST",
			Path:         resolveModeratorPath(payload),
			Query:        "",
			Headers:      map[string]string{},
			Body:         mustJSON(payload),
		})
		dispatchRows = append(dispatchRows, map[string]any{
			"member_id":     member.MemberID,
			"agent_id":      member.AgentID,
			"deployment_id": member.DeploymentID,
			"session_id":    sessionID,
		})
	}

	if len(tasks) == 0 {
		return nil, skippedRows, errors.New("no eligible member sessions")
	}

	conc := 4
	if maxConcurrency != nil && *maxConcurrency > 0 {
		conc = *maxConcurrency
	}
	if conc > 16 {
		conc = 16
	}
	tmo := 10_000
	if timeoutMS != nil && *timeoutMS > 0 {
		tmo = *timeoutMS
	}
	results := s.executeAgentTasks(r.Context(), p, tasks, conc, tmo, traceIDFromContext(r.Context()))
	for idx, res := range results {
		if idx >= len(dispatchRows) {
			continue
		}
		dispatchRows[idx]["ok"] = res.OK
		if res.HTTPStatus != 0 {
			dispatchRows[idx]["http_status"] = res.HTTPStatus
		}
		if res.Error != "" {
			dispatchRows[idx]["error"] = res.Error
		}
	}
	return dispatchRows, skippedRows, nil
}

func resolveModeratorPath(payload map[string]any) string {
	if _, ok := payload["directive"]; ok {
		return "/api/v1/moderator/directive"
	}
	return "/api/v1/moderator/task"
}

func filterTeamRunModeratorTargets(members []teamRunMemberInfo, targets *teamRunModeratorTargets) ([]teamRunMemberInfo, []map[string]any) {
	if len(members) == 0 {
		return nil, nil
	}
	if targets == nil {
		return members, nil
	}
	roleSet := map[string]bool{}
	for _, r := range targets.Roles {
		r = strings.ToLower(strings.TrimSpace(r))
		if r != "" {
			roleSet[r] = true
		}
	}
	memberSet := map[string]bool{}
	for _, m := range targets.MemberIDs {
		m = strings.TrimSpace(m)
		if m != "" {
			memberSet[m] = true
		}
	}
	agentSet := map[string]bool{}
	for _, a := range targets.AgentIDs {
		a = strings.TrimSpace(a)
		if a != "" {
			agentSet[a] = true
		}
	}
	hasTargets := len(roleSet) > 0 || len(memberSet) > 0 || len(agentSet) > 0
	if !hasTargets {
		return members, nil
	}
	selected := make([]teamRunMemberInfo, 0, len(members))
	skipped := make([]map[string]any, 0)
	for _, m := range members {
		match := false
		if len(memberSet) > 0 && memberSet[m.MemberID] {
			match = true
		}
		if len(agentSet) > 0 && agentSet[m.AgentID] {
			match = true
		}
		if len(roleSet) > 0 {
			role := strings.ToLower(strings.TrimSpace(m.Role))
			if roleSet[role] {
				match = true
			}
		}
		if match {
			selected = append(selected, m)
		} else {
			skipped = append(skipped, map[string]any{
				"member_id": m.MemberID,
				"agent_id":  m.AgentID,
				"reason":    "target_filter",
			})
		}
	}
	return selected, skipped
}

func parseTeamRunModeratorTargets(q url.Values) *teamRunModeratorTargets {
	if q == nil {
		return nil
	}
	roles := splitCSVParam(q.Get("roles"))
	memberIDs := splitCSVParam(q.Get("member_ids"))
	agentIDs := splitCSVParam(q.Get("agent_ids"))
	if len(roles) == 0 && len(memberIDs) == 0 && len(agentIDs) == 0 {
		return nil
	}
	return &teamRunModeratorTargets{
		Roles:     roles,
		MemberIDs: memberIDs,
		AgentIDs:  agentIDs,
	}
}

func splitCSVParam(raw string) []string {
	raw = strings.TrimSpace(raw)
	if raw == "" {
		return nil
	}
	parts := strings.Split(raw, ",")
	out := make([]string, 0, len(parts))
	for _, part := range parts {
		part = strings.TrimSpace(part)
		if part == "" {
			continue
		}
		out = append(out, part)
	}
	if len(out) == 0 {
		return nil
	}
	return out
}
