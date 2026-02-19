package broker

import (
	"encoding/json"
	"net/http"
	"net/url"
	"sort"
	"strings"
)

func (s *Server) handleAgents(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case "GET":
		s.handleAgentsList(w, r)
	case "POST":
		s.handleAgentsCreate(w, r)
	default:
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
	}
}

func (s *Server) handleAgentsList(w http.ResponseWriter, r *http.Request) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if p.AuthKind != "oidc" {
		writeErrorJSON(w, "oidc required for agent listing", http.StatusForbidden)
		return
	}
	type DeploymentInfo struct {
		DeploymentID string         `json:"deployment_id"`
		Connected    bool           `json:"connected"`
		ConnectedAt  int64          `json:"connected_unix_ms,omitempty"`
		LastSeen     int64          `json:"last_seen_unix_ms,omitempty"`
		RemoteAddr   string         `json:"remote_addr,omitempty"`
		Meta         map[string]any `json:"meta,omitempty"`
	}
	type AgentInfo struct {
		AgentID     string           `json:"agent_id"`
		OwnerSub    string           `json:"owner_sub"`
		Enabled     bool             `json:"enabled"`
		CreatedAt   int64            `json:"created_unix_ms"`
		Labels      map[string]any   `json:"labels,omitempty"`
		Meta        map[string]any   `json:"meta,omitempty"`
		Connected   bool             `json:"connected"`
		ConnectedAt int64            `json:"connected_unix_ms,omitempty"`
		LastSeen    int64            `json:"last_seen_unix_ms,omitempty"`
		RemoteAddr  string           `json:"remote_addr,omitempty"`
		Deployments []DeploymentInfo `json:"deployments,omitempty"`
	}
	dbAgents, err := s.cfg.DB.ListAgentsForUser(r.Context(), p.Sub)
	if err != nil {
		writeErrorJSON(w, "db error", http.StatusInternalServerError)
		return
	}
	out := make([]AgentInfo, 0, len(dbAgents))
	for _, a := range dbAgents {
		info := AgentInfo{
			AgentID:   a.AgentID,
			OwnerSub:  a.OwnerSub,
			Enabled:   a.Enabled,
			CreatedAt: a.CreatedAt.UnixMilli(),
			Labels:    a.Labels(),
			Meta:      a.Meta(),
		}
		deployments := s.cfg.Registry.ListByAgent(a.AgentID)
		if len(deployments) > 0 {
			sort.Slice(deployments, func(i, j int) bool {
				return deployments[i].Connected.After(deployments[j].Connected)
			})
			info.Connected = true
			info.ConnectedAt = deployments[0].Connected.UnixMilli()
			info.LastSeen = deployments[0].LastSeen.UnixMilli()
			info.RemoteAddr = deployments[0].RemoteAddr
			for _, ac := range deployments {
				if ac == nil {
					continue
				}
				info.Deployments = append(info.Deployments, DeploymentInfo{
					DeploymentID: ac.DeploymentID,
					Connected:    true,
					ConnectedAt:  ac.Connected.UnixMilli(),
					LastSeen:     ac.LastSeen.UnixMilli(),
					RemoteAddr:   ac.RemoteAddr,
					Meta:         ac.Meta,
				})
			}
		}
		out = append(out, info)
	}
	writeJSON(w, map[string]any{"ok": true, "agents": out})
}

func (s *Server) handleAgentsCreate(w http.ResponseWriter, r *http.Request) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if p.AuthKind != "oidc" {
		writeErrorJSON(w, "oidc required for agent creation", http.StatusForbidden)
		return
	}
	body, err := readBodyBounded(r.Body, 1024*1024)
	if err != nil {
		writeErrorJSON(w, "invalid body", http.StatusBadRequest)
		return
	}
	req := struct {
		AgentID string         `json:"agent_id"`
		Labels  map[string]any `json:"labels,omitempty"`
		Meta    map[string]any `json:"meta,omitempty"`
	}{}
	if len(body) > 0 {
		if err := json.Unmarshal(body, &req); err != nil {
			writeErrorJSON(w, "invalid json", http.StatusBadRequest)
			return
		}
	}
	agentID := strings.TrimSpace(req.AgentID)
	if agentID == "" {
		agentID = "a-" + newID()[:12]
	}
	if !agentIDRe.MatchString(agentID) {
		writeErrorJSON(w, "invalid agent_id", http.StatusBadRequest)
		return
	}
	if p.Admin && strings.TrimSpace(req.AgentID) != "" && strings.HasPrefix(agentID, "a-") {
		// no-op: allow admin to set ids freely
	}

	a, err := s.cfg.DB.CreateAgent(r.Context(), p.Sub, agentID, req.Labels, req.Meta)
	if err != nil {
		writeErrorJSON(w, "create agent failed", http.StatusBadRequest)
		return
	}
	writeJSON(w, map[string]any{
		"ok": true,
		"agent": map[string]any{
			"agent_id":          a.AgentID,
			"owner_sub":         a.OwnerSub,
			"enabled":           a.Enabled,
			"created_unix_ms":   a.CreatedAt.UnixMilli(),
			"labels":            a.Labels(),
			"meta":              a.Meta(),
			"connector_hint_cn": s.cfg.AgentCNPfx + a.AgentID,
		},
	})
}

func (s *Server) handleAgentsSubroutes(w http.ResponseWriter, r *http.Request) {
	// Supported:
	// - /v1/agents/{agent_id}/proxy/<agentd_path>
	// - /v1/agents/{agent_id}/proxy_sse/<agentd_path>
	// - /v1/agents/{agent_id}/disconnect
	// - /v1/agents/{agent_id}/deployments
	// - /v1/agents/{agent_id}/ota/update
	// - /v1/agents/{agent_id}/ota/status
	// - /v1/agents/{agent_id}/memory/retention/enforce
	// - /v1/agents/{agent_id}/memory/recaps
	// - /v1/agents/{agent_id}/memory/salience
	// - /v1/agents/{agent_id}/members
	// - /v1/agents/{agent_id}/members/{user_sub}
	// - /v1/agents/{agent_id}/membership_audit

	rest := strings.TrimPrefix(r.URL.Path, "/v1/agents/")
	parts := strings.SplitN(rest, "/", 3)
	if len(parts) < 2 {
		writeErrorJSON(w, "not found", http.StatusNotFound)
		return
	}
	agentID := parts[0]
	action := parts[1]

	if action == "disconnect" {
		s.handleAgentDisconnect(w, r, agentID)
		return
	}
	if action == "deployments" {
		s.handleAgentDeployments(w, r, agentID)
		return
	}
	if action == "delete" {
		s.handleAgentDelete(w, r, agentID)
		return
	}
	if action == "proxy" {
		agentPath := "/"
		if len(parts) == 3 && parts[2] != "" {
			agentPath = "/" + parts[2]
		}
		s.handleAgentProxy(w, r, agentID, agentPath)
		return
	}
	if action == "ota" {
		if len(parts) < 3 || parts[2] == "" {
			writeErrorJSON(w, "not found", http.StatusNotFound)
			return
		}
		switch parts[2] {
		case "update":
			s.handleAgentOtaUpdateBulk(w, r, agentID)
		case "status":
			s.handleAgentOtaStatusBulk(w, r, agentID)
		default:
			writeErrorJSON(w, "not found", http.StatusNotFound)
		}
		return
	}
	if action == "memory" {
		if len(parts) < 3 || parts[2] == "" {
			writeErrorJSON(w, "not found", http.StatusNotFound)
			return
		}
		switch parts[2] {
		case "retention/enforce":
			s.handleAgentMemoryRetentionBulk(w, r, agentID)
		case "recaps":
			s.handleAgentMemoryRecapsBulk(w, r, agentID)
		case "salience":
			s.handleAgentMemorySalienceBulk(w, r, agentID)
		default:
			writeErrorJSON(w, "not found", http.StatusNotFound)
		}
		return
	}
	if action == "proxy_sse" {
		agentPath := "/"
		if len(parts) == 3 && parts[2] != "" {
			agentPath = "/" + parts[2]
		}
		s.handleAgentProxySSE(w, r, agentID, agentPath)
		return
	}
	if action == "members" {
		if len(parts) == 2 || parts[2] == "" {
			switch r.Method {
			case "GET":
				s.handleAgentMembersList(w, r, agentID)
			case "POST":
				s.handleAgentMembersUpsert(w, r, agentID)
			default:
				writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
			}
			return
		}
		userSub, err := url.PathUnescape(parts[2])
		if err != nil {
			writeErrorJSON(w, "invalid user_sub", http.StatusBadRequest)
			return
		}
		s.handleAgentMemberDelete(w, r, agentID, userSub)
		return
	}
	if action == "membership_audit" {
		s.handleAgentMembershipAudit(w, r, agentID)
		return
	}

	writeErrorJSON(w, "not found", http.StatusNotFound)
}

func (s *Server) handleAgentDisconnect(w http.ResponseWriter, r *http.Request, agentID string) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if !p.Admin {
		writeErrorJSON(w, "admin required", http.StatusForbidden)
		return
	}
	if r.Method != "POST" {
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	deploymentID, derr := deploymentIDFromRequest(r)
	if derr != nil {
		writeErrorJSON(w, derr.Error(), http.StatusBadRequest)
		return
	}
	deleted := s.cfg.Registry.Delete(agentID, deploymentID)
	if len(deleted) == 0 {
		writeErrorJSON(w, "agent not connected", http.StatusNotFound)
		return
	}
	for _, a := range deleted {
		if a != nil {
			a.Close()
		}
	}
	writeJSON(w, map[string]any{"ok": true})
}

func (s *Server) handleAgentDeployments(w http.ResponseWriter, r *http.Request, agentID string) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if r.Method != "GET" {
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if ok, err := s.canAccessAgent(r.Context(), p, agentID); err != nil {
		writeErrorJSON(w, "db error", http.StatusInternalServerError)
		return
	} else if !ok {
		writeErrorJSON(w, "forbidden", http.StatusForbidden)
		return
	}
	type DeploymentInfo struct {
		DeploymentID string         `json:"deployment_id"`
		Connected    bool           `json:"connected"`
		ConnectedAt  int64          `json:"connected_unix_ms,omitempty"`
		LastSeen     int64          `json:"last_seen_unix_ms,omitempty"`
		RemoteAddr   string         `json:"remote_addr,omitempty"`
		Meta         map[string]any `json:"meta,omitempty"`
	}

	deployments := s.cfg.Registry.ListByAgent(agentID)
	sort.Slice(deployments, func(i, j int) bool {
		return deployments[i].Connected.After(deployments[j].Connected)
	})
	out := make([]DeploymentInfo, 0, len(deployments))
	for _, ac := range deployments {
		if ac == nil {
			continue
		}
		out = append(out, DeploymentInfo{
			DeploymentID: ac.DeploymentID,
			Connected:    true,
			ConnectedAt:  ac.Connected.UnixMilli(),
			LastSeen:     ac.LastSeen.UnixMilli(),
			RemoteAddr:   ac.RemoteAddr,
			Meta:         ac.Meta,
		})
	}
	defaultID := ""
	if ac, ok := s.cfg.Registry.Select(agentID, ""); ok && ac != nil {
		defaultID = ac.DeploymentID
	}
	writeJSON(w, map[string]any{
		"ok":                    true,
		"agent_id":              agentID,
		"default_deployment_id": defaultID,
		"deployments":           out,
	})
}

func (s *Server) handleAgentDelete(w http.ResponseWriter, r *http.Request, agentID string) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if p.AuthKind == "client" && !p.Admin {
		writeErrorJSON(w, "client auth cannot delete agents", http.StatusForbidden)
		return
	}
	if r.Method != "POST" && r.Method != "DELETE" {
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if p.Admin {
		if err := s.cfg.DB.DeleteAgent(r.Context(), agentID); err != nil {
			writeErrorJSON(w, "delete failed", http.StatusBadRequest)
			return
		}
		writeJSON(w, map[string]any{"ok": true})
		return
	}
	if err := s.cfg.DB.DeleteAgentIfOwner(r.Context(), p.Sub, agentID); err != nil {
		writeErrorJSON(w, "not owner", http.StatusForbidden)
		return
	}
	writeJSON(w, map[string]any{"ok": true})
}
