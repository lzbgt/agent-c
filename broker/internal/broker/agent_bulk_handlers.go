package broker

import (
	"context"
	"encoding/json"
	"net/http"
	"strings"
	"sync"
)

type agentFanoutResult struct {
	DeploymentID string `json:"deployment_id"`
	Status       int    `json:"status"`
	Data         any    `json:"data"`
}

func (s *Server) relayAgentHTTPBulk(
	ctx context.Context,
	p *Principal,
	agentID string,
	deploymentIDs []string,
	method string,
	agentPath string,
	rawQuery string,
	headers map[string]string,
	body []byte,
) []agentFanoutResult {
	if len(deploymentIDs) == 0 {
		return nil
	}
	const maxFanout = 4
	sem := make(chan struct{}, maxFanout)
	out := make([]agentFanoutResult, len(deploymentIDs))
	var wg sync.WaitGroup
	for i, dep := range deploymentIDs {
		i := i
		dep := dep
		wg.Add(1)
		go func() {
			defer wg.Done()
			if maxFanout > 0 {
				sem <- struct{}{}
				defer func() { <-sem }()
			}
			ro := s.relayAgentHTTP(ctx, p, agentID, dep, method, agentPath, rawQuery, headers, body)
			if ro.BrokerStatus != 0 {
				out[i] = agentFanoutResult{
					DeploymentID: dep,
					Status:       ro.BrokerStatus,
					Data: map[string]any{
						"ok":            false,
						"error":         ro.Err,
						"broker_status": ro.BrokerStatus,
					},
				}
				return
			}
			if len(ro.Body) == 0 {
				out[i] = agentFanoutResult{
					DeploymentID: dep,
					Status:       ro.AgentStatus,
					Data: map[string]any{
						"ok":    false,
						"error": "empty response",
					},
				}
				return
			}
			var data any
			if err := json.Unmarshal(ro.Body, &data); err != nil {
				out[i] = agentFanoutResult{
					DeploymentID: dep,
					Status:       ro.AgentStatus,
					Data: map[string]any{
						"ok":    false,
						"error": "invalid agent response",
						"raw":   string(ro.Body),
					},
				}
				return
			}
			out[i] = agentFanoutResult{
				DeploymentID: dep,
				Status:       ro.AgentStatus,
				Data:         data,
			}
		}()
	}
	wg.Wait()
	return out
}

func (s *Server) handleAgentOtaUpdateBulk(w http.ResponseWriter, r *http.Request, agentID string) {
	if r.Method != "POST" {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	p, err := s.requirePrincipal(r)
	if err != nil {
		http.Error(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if ok, err := s.canAccessAgent(r.Context(), p, agentID); err != nil {
		http.Error(w, "db error", http.StatusInternalServerError)
		return
	} else if !ok {
		http.Error(w, "forbidden", http.StatusForbidden)
		return
	}

	body, err := readBodyBounded(r.Body, s.cfg.MaxRequestBodySize)
	if err != nil {
		http.Error(w, "request body too large", http.StatusRequestEntityTooLarge)
		return
	}
	deploymentIDs, err := deploymentIDsFromBody(body)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	if len(deploymentIDs) == 0 {
		deploymentIDs, err = deploymentIDsFromQuery(r)
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
	}
	if len(deploymentIDs) == 0 {
		for _, ac := range s.cfg.Registry.ListByAgent(agentID) {
			if ac == nil || ac.DeploymentID == "" {
				continue
			}
			deploymentIDs = append(deploymentIDs, ac.DeploymentID)
		}
	}
	if len(deploymentIDs) == 0 {
		http.Error(w, "agent not connected", http.StatusNotFound)
		return
	}

	headers := buildAgentForwardHeaders(r, p.Sub)
	hasContentType := false
	for k := range headers {
		if strings.EqualFold(k, "Content-Type") {
			hasContentType = true
			break
		}
	}
	if !hasContentType {
		headers["Content-Type"] = "application/json"
	}

	results := s.relayAgentHTTPBulk(r.Context(), p, agentID, deploymentIDs, "POST", "/api/v1/ota/update", "", headers, body)
	okCount := 0
	for _, res := range results {
		if m, ok := res.Data.(map[string]any); ok {
			if v, ok := m["ok"].(bool); ok && v {
				okCount++
			}
		}
	}
	writeJSON(w, map[string]any{
		"ok":          okCount == len(results),
		"agent_id":    agentID,
		"results":     results,
		"total":       len(results),
		"ok_count":    okCount,
		"error_count": len(results) - okCount,
	})
}

func (s *Server) handleAgentOtaStatusBulk(w http.ResponseWriter, r *http.Request, agentID string) {
	if r.Method != "GET" {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	p, err := s.requirePrincipal(r)
	if err != nil {
		http.Error(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if ok, err := s.canAccessAgent(r.Context(), p, agentID); err != nil {
		http.Error(w, "db error", http.StatusInternalServerError)
		return
	} else if !ok {
		http.Error(w, "forbidden", http.StatusForbidden)
		return
	}
	deploymentIDs, err := deploymentIDsFromQuery(r)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	if len(deploymentIDs) == 0 {
		for _, ac := range s.cfg.Registry.ListByAgent(agentID) {
			if ac == nil || ac.DeploymentID == "" {
				continue
			}
			deploymentIDs = append(deploymentIDs, ac.DeploymentID)
		}
	}
	if len(deploymentIDs) == 0 {
		http.Error(w, "agent not connected", http.StatusNotFound)
		return
	}

	headers := buildAgentForwardHeaders(r, p.Sub)
	results := s.relayAgentHTTPBulk(r.Context(), p, agentID, deploymentIDs, "GET", "/api/v1/ota/status", "", headers, nil)
	okCount := 0
	for _, res := range results {
		if m, ok := res.Data.(map[string]any); ok {
			if v, ok := m["ok"].(bool); ok && v {
				okCount++
			}
		}
	}
	writeJSON(w, map[string]any{
		"ok":          okCount == len(results),
		"agent_id":    agentID,
		"results":     results,
		"total":       len(results),
		"ok_count":    okCount,
		"error_count": len(results) - okCount,
	})
}

func (s *Server) handleAgentMemoryRetentionBulk(w http.ResponseWriter, r *http.Request, agentID string) {
	if r.Method != "POST" {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	p, err := s.requirePrincipal(r)
	if err != nil {
		http.Error(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if ok, err := s.canAccessAgent(r.Context(), p, agentID); err != nil {
		http.Error(w, "db error", http.StatusInternalServerError)
		return
	} else if !ok {
		http.Error(w, "forbidden", http.StatusForbidden)
		return
	}

	body, err := readBodyBounded(r.Body, s.cfg.MaxRequestBodySize)
	if err != nil {
		http.Error(w, "request body too large", http.StatusRequestEntityTooLarge)
		return
	}
	deploymentIDs, err := deploymentIDsFromBody(body)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	if len(deploymentIDs) == 0 {
		deploymentIDs, err = deploymentIDsFromQuery(r)
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
	}
	if len(deploymentIDs) == 0 {
		for _, ac := range s.cfg.Registry.ListByAgent(agentID) {
			if ac == nil || ac.DeploymentID == "" {
				continue
			}
			deploymentIDs = append(deploymentIDs, ac.DeploymentID)
		}
	}
	if len(deploymentIDs) == 0 {
		http.Error(w, "agent not connected", http.StatusNotFound)
		return
	}

	headers := buildAgentForwardHeaders(r, p.Sub)
	hasContentType := false
	for k := range headers {
		if strings.EqualFold(k, "Content-Type") {
			hasContentType = true
			break
		}
	}
	if !hasContentType {
		headers["Content-Type"] = "application/json"
	}

	results := s.relayAgentHTTPBulk(r.Context(), p, agentID, deploymentIDs, "POST", "/api/v1/memory/retention/enforce", "", headers, body)
	okCount := 0
	for _, res := range results {
		if m, ok := res.Data.(map[string]any); ok {
			if v, ok := m["ok"].(bool); ok && v {
				okCount++
			}
		}
	}
	writeJSON(w, map[string]any{
		"ok":          okCount == len(results),
		"agent_id":    agentID,
		"results":     results,
		"total":       len(results),
		"ok_count":    okCount,
		"error_count": len(results) - okCount,
	})
}

func (s *Server) handleAgentMemoryRecapsBulk(w http.ResponseWriter, r *http.Request, agentID string) {
	if r.Method != "GET" && r.Method != "POST" {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	p, err := s.requirePrincipal(r)
	if err != nil {
		http.Error(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if ok, err := s.canAccessAgent(r.Context(), p, agentID); err != nil {
		http.Error(w, "db error", http.StatusInternalServerError)
		return
	} else if !ok {
		http.Error(w, "forbidden", http.StatusForbidden)
		return
	}

	var body []byte
	if r.Method == "POST" {
		body, err = readBodyBounded(r.Body, s.cfg.MaxRequestBodySize)
		if err != nil {
			http.Error(w, "request body too large", http.StatusRequestEntityTooLarge)
			return
		}
	}
	deploymentIDs, derr := deploymentIDsFromBody(body)
	if derr != nil {
		http.Error(w, derr.Error(), http.StatusBadRequest)
		return
	}
	if len(deploymentIDs) == 0 {
		deploymentIDs, derr = deploymentIDsFromQuery(r)
		if derr != nil {
			http.Error(w, derr.Error(), http.StatusBadRequest)
			return
		}
	}
	if len(deploymentIDs) == 0 {
		for _, ac := range s.cfg.Registry.ListByAgent(agentID) {
			if ac == nil || ac.DeploymentID == "" {
				continue
			}
			deploymentIDs = append(deploymentIDs, ac.DeploymentID)
		}
	}
	if len(deploymentIDs) == 0 {
		http.Error(w, "agent not connected", http.StatusNotFound)
		return
	}

	headers := buildAgentForwardHeaders(r, p.Sub)
	if r.Method == "POST" {
		hasContentType := false
		for k := range headers {
			if strings.EqualFold(k, "Content-Type") {
				hasContentType = true
				break
			}
		}
		if !hasContentType {
			headers["Content-Type"] = "application/json"
		}
	}

	rawQuery := ""
	if r.Method == "GET" {
		rawQuery = r.URL.RawQuery
	}
	results := s.relayAgentHTTPBulk(r.Context(), p, agentID, deploymentIDs, r.Method, "/api/v1/memory/recaps", rawQuery, headers, body)
	okCount := 0
	for _, res := range results {
		if m, ok := res.Data.(map[string]any); ok {
			if v, ok := m["ok"].(bool); ok && v {
				okCount++
			}
		}
	}
	writeJSON(w, map[string]any{
		"ok":          okCount == len(results),
		"agent_id":    agentID,
		"results":     results,
		"total":       len(results),
		"ok_count":    okCount,
		"error_count": len(results) - okCount,
	})
}

func (s *Server) handleAgentMemorySalienceBulk(w http.ResponseWriter, r *http.Request, agentID string) {
	if r.Method != "GET" {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	p, err := s.requirePrincipal(r)
	if err != nil {
		http.Error(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if ok, err := s.canAccessAgent(r.Context(), p, agentID); err != nil {
		http.Error(w, "db error", http.StatusInternalServerError)
		return
	} else if !ok {
		http.Error(w, "forbidden", http.StatusForbidden)
		return
	}

	deploymentIDs, derr := deploymentIDsFromQuery(r)
	if derr != nil {
		http.Error(w, derr.Error(), http.StatusBadRequest)
		return
	}
	if len(deploymentIDs) == 0 {
		for _, ac := range s.cfg.Registry.ListByAgent(agentID) {
			if ac == nil || ac.DeploymentID == "" {
				continue
			}
			deploymentIDs = append(deploymentIDs, ac.DeploymentID)
		}
	}
	if len(deploymentIDs) == 0 {
		http.Error(w, "agent not connected", http.StatusNotFound)
		return
	}

	headers := buildAgentForwardHeaders(r, p.Sub)
	rawQuery := r.URL.RawQuery
	results := s.relayAgentHTTPBulk(r.Context(), p, agentID, deploymentIDs, "GET", "/api/v1/memory/salience", rawQuery, headers, nil)
	okCount := 0
	for _, res := range results {
		if m, ok := res.Data.(map[string]any); ok {
			if v, ok := m["ok"].(bool); ok && v {
				okCount++
			}
		}
	}
	writeJSON(w, map[string]any{
		"ok":          okCount == len(results),
		"agent_id":    agentID,
		"results":     results,
		"total":       len(results),
		"ok_count":    okCount,
		"error_count": len(results) - okCount,
	})
}
