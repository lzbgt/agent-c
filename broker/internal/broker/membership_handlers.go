package broker

import (
	"encoding/json"
	"net/http"
	"strings"

	"agentd-broker/internal/db"
	"agentd-broker/internal/events"
)

func (s *Server) handleAgentMembersList(w http.ResponseWriter, r *http.Request, agentID string) {
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
	ownerSub, err := s.cfg.DB.GetAgentOwnerSub(r.Context(), agentID)
	if err != nil {
		writeErrorJSON(w, "agent not found", http.StatusNotFound)
		return
	}
	if !p.Admin && p.Sub != ownerSub {
		writeErrorJSON(w, "not owner", http.StatusForbidden)
		return
	}
	members, err := s.cfg.DB.ListAgentMembers(r.Context(), agentID)
	if err != nil {
		writeErrorJSON(w, "db error", http.StatusInternalServerError)
		return
	}
	out := make([]map[string]any, 0, len(members))
	for _, m := range members {
		out = append(out, map[string]any{
			"user_sub":        m.UserSub,
			"role":            m.Role,
			"created_unix_ms": m.CreatedAt.UnixMilli(),
		})
	}
	writeJSON(w, map[string]any{
		"ok":        true,
		"agent_id":  agentID,
		"owner_sub": ownerSub,
		"members":   out,
	})
}

func (s *Server) handleAgentMembersUpsert(w http.ResponseWriter, r *http.Request, agentID string) {
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
	ownerSub, err := s.cfg.DB.GetAgentOwnerSub(r.Context(), agentID)
	if err != nil {
		writeErrorJSON(w, "agent not found", http.StatusNotFound)
		return
	}
	if !p.Admin && p.Sub != ownerSub {
		writeErrorJSON(w, "not owner", http.StatusForbidden)
		return
	}
	body, err := readBodyBounded(r.Body, 1024*1024)
	if err != nil {
		writeErrorJSON(w, "invalid body", http.StatusBadRequest)
		return
	}
	req := struct {
		UserSub string `json:"user_sub"`
		Role    string `json:"role"`
	}{}
	if len(body) > 0 {
		if err := json.Unmarshal(body, &req); err != nil {
			writeErrorJSON(w, "invalid json", http.StatusBadRequest)
			return
		}
	}
	userSub := strings.TrimSpace(req.UserSub)
	if userSub == "" {
		writeErrorJSON(w, "missing user_sub", http.StatusBadRequest)
		return
	}
	role := strings.ToLower(strings.TrimSpace(req.Role))
	if role == "" {
		role = "user"
	}
	if userSub == ownerSub {
		role = "owner"
	}
	if role != "user" && role != "admin" && role != "owner" {
		writeErrorJSON(w, "invalid role", http.StatusBadRequest)
		return
	}
	if role == "owner" && userSub != ownerSub {
		writeErrorJSON(w, "owner role only for agent owner", http.StatusForbidden)
		return
	}
	if err := s.cfg.DB.UpsertAgentMember(r.Context(), agentID, userSub, role); err != nil {
		writeErrorJSON(w, "upsert failed", http.StatusBadRequest)
		return
	}
	_ = s.cfg.DB.InsertMembershipAudit(r.Context(), p.Sub, userSub, agentID, "upsert", role, traceIDFromContext(r.Context()))
	if subs, err := s.cfg.DB.ListAgentMemberSubs(r.Context(), agentID); err == nil && len(subs) > 0 {
		s.cfg.Events.PublishTo(subs, events.Event{
			Type:    "agent_member_updated",
			AgentID: agentID,
			UserSub: userSub,
			Payload: map[string]any{
				"actor_sub": p.Sub,
				"role":      role,
				"action":    "upsert",
			},
		})
	}
	writeJSON(w, map[string]any{"ok": true})
}

func (s *Server) handleAgentMemberDelete(w http.ResponseWriter, r *http.Request, agentID, userSub string) {
	if r.Method != "DELETE" {
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
	ownerSub, err := s.cfg.DB.GetAgentOwnerSub(r.Context(), agentID)
	if err != nil {
		writeErrorJSON(w, "agent not found", http.StatusNotFound)
		return
	}
	if !p.Admin && p.Sub != ownerSub {
		writeErrorJSON(w, "not owner", http.StatusForbidden)
		return
	}
	userSub = strings.TrimSpace(userSub)
	if userSub == "" {
		writeErrorJSON(w, "missing user_sub", http.StatusBadRequest)
		return
	}
	if userSub == ownerSub {
		writeErrorJSON(w, "cannot remove owner", http.StatusForbidden)
		return
	}
	ok, err := s.cfg.DB.RemoveAgentMember(r.Context(), agentID, userSub)
	if err != nil {
		writeErrorJSON(w, "delete failed", http.StatusInternalServerError)
		return
	}
	if !ok {
		writeErrorJSON(w, "not found", http.StatusNotFound)
		return
	}
	_ = s.cfg.DB.InsertMembershipAudit(r.Context(), p.Sub, userSub, agentID, "remove", "", traceIDFromContext(r.Context()))
	if subs, err := s.cfg.DB.ListAgentMemberSubs(r.Context(), agentID); err == nil && len(subs) > 0 {
		s.cfg.Events.PublishTo(subs, events.Event{
			Type:    "agent_member_updated",
			AgentID: agentID,
			UserSub: userSub,
			Payload: map[string]any{
				"actor_sub": p.Sub,
				"action":    "remove",
			},
		})
	}
	writeJSON(w, map[string]any{"ok": true})
}

func (s *Server) handleAgentMembershipAudit(w http.ResponseWriter, r *http.Request, agentID string) {
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
	ownerSub, err := s.cfg.DB.GetAgentOwnerSub(r.Context(), agentID)
	if err != nil {
		writeErrorJSON(w, "agent not found", http.StatusNotFound)
		return
	}
	if !p.Admin && p.Sub != ownerSub {
		writeErrorJSON(w, "not owner", http.StatusForbidden)
		return
	}
	limit := 200
	if v := strings.TrimSpace(r.URL.Query().Get("limit")); v != "" {
		if n, ok := parseIntBounded(v, 1, 500); ok {
			limit = n
		}
	}
	var rows []db.MembershipAudit
	if p.Admin {
		rows, err = s.cfg.DB.ListMembershipAuditByAgentAdmin(r.Context(), agentID, limit)
	} else {
		rows, err = s.cfg.DB.ListMembershipAuditByAgent(r.Context(), p.Sub, agentID, limit)
	}
	if err != nil {
		writeErrorJSON(w, "db error", http.StatusInternalServerError)
		return
	}
	out := make([]map[string]any, 0, len(rows))
	for _, row := range rows {
		entry := map[string]any{
			"ts_unix_ms": row.TSUnixMS,
			"actor_sub":  row.ActorSub,
			"target_sub": row.TargetSub,
			"action":     row.Action,
		}
		if row.Role != "" {
			entry["role"] = row.Role
		}
		if row.TraceID != "" {
			entry["trace_id"] = row.TraceID
		}
		out = append(out, entry)
	}
	writeJSON(w, map[string]any{
		"ok":        true,
		"agent_id":  agentID,
		"owner_sub": ownerSub,
		"audit":     out,
	})
}
