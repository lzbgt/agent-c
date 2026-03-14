package broker

import (
	"encoding/json"
	"net/http"
	"net/url"
	"path"
	"strings"
)

func (s *Server) handleAgentSessionsSubroutes(w http.ResponseWriter, r *http.Request, agentID string, topParts []string) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if ok, err := s.canAccessAgent(r.Context(), p, agentID); err != nil {
		writeErrorJSON(w, "db error", http.StatusInternalServerError)
		return
	} else if !ok {
		writeErrorJSON(w, "forbidden", http.StatusForbidden)
		return
	}
	deploymentID, derr := deploymentIDFromRequest(r)
	if derr != nil {
		writeErrorJSON(w, derr.Error(), http.StatusBadRequest)
		return
	}

	suffix := ""
	if len(topParts) >= 3 {
		suffix = strings.Trim(topParts[2], "/")
	}
	if suffix == "" {
		s.handleAgentSessionsRootAlias(w, r, p, agentID, deploymentID)
		return
	}

	parts := strings.Split(suffix, "/")
	if len(parts) == 0 || strings.TrimSpace(parts[0]) == "" {
		writeErrorJSON(w, "invalid session_id", http.StatusBadRequest)
		return
	}
	sessionID, err := url.PathUnescape(parts[0])
	if err != nil || strings.TrimSpace(sessionID) == "" {
		writeErrorJSON(w, "invalid session_id", http.StatusBadRequest)
		return
	}
	if len(parts) == 1 {
		s.handleAgentSessionAlias(w, r, p, agentID, deploymentID, sessionID)
		return
	}
	switch parts[1] {
	case "attach":
		if len(parts) != 2 {
			writeErrorJSON(w, "not found", http.StatusNotFound)
			return
		}
		s.handleAgentSessionAttachAlias(w, r, p, agentID, deploymentID, sessionID)
		return
	case "attachment":
		if len(parts) != 3 {
			writeErrorJSON(w, "not found", http.StatusNotFound)
			return
		}
		switch parts[2] {
		case "renew":
			s.handleAgentSessionAttachmentAlias(w, r, p, agentID, deploymentID, sessionID, "/api/v1/session/"+sessionID+"/attachment/renew")
		case "release":
			s.handleAgentSessionAttachmentAlias(w, r, p, agentID, deploymentID, sessionID, "/api/v1/session/"+sessionID+"/attachment/release")
		default:
			writeErrorJSON(w, "not found", http.StatusNotFound)
		}
		return
	case "events":
		if len(parts) != 2 {
			writeErrorJSON(w, "not found", http.StatusNotFound)
			return
		}
		if r.Method != "GET" {
			writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		s.handleAgentSessionEventsAlias(w, r, agentID, "/api/v1/session/"+sessionID+"/events")
		return
	case "transcript":
		if len(parts) != 2 {
			writeErrorJSON(w, "not found", http.StatusNotFound)
			return
		}
		s.handleAgentSessionTranscriptAlias(w, r, p, agentID, deploymentID, sessionID)
		return
	case "orchestration":
		if len(parts) != 3 {
			writeErrorJSON(w, "not found", http.StatusNotFound)
			return
		}
		switch parts[2] {
		case "status", "workers", "dependencies":
			s.handleAgentSessionReadAlias(
				w,
				r,
				p,
				agentID,
				deploymentID,
				path.Join("/api/v1/session", sessionID, "orchestration", parts[2]),
			)
		default:
			writeErrorJSON(w, "not found", http.StatusNotFound)
		}
		return
	case "shells":
		s.handleAgentSessionShellsAlias(w, r, p, agentID, deploymentID, sessionID, parts[2:])
		return
	case "services":
		s.handleAgentSessionServicesAlias(w, r, p, agentID, deploymentID, sessionID, parts[2:])
		return
	case "capabilities":
		s.handleAgentSessionCapabilitiesAlias(w, r, p, agentID, deploymentID, sessionID, parts[2:])
		return
	default:
		writeErrorJSON(w, "not found", http.StatusNotFound)
		return
	}
}

func (s *Server) handleAgentSessionsRootAlias(w http.ResponseWriter, r *http.Request, p *Principal, agentID, deploymentID string) {
	switch r.Method {
	case "GET":
		s.relayAuthorizedAgentHTTP(w, r, p, agentID, deploymentID, r.Method, "/api/v1/session", r.URL.RawQuery, nil, "")
	case "POST":
		body, err := readBodyBounded(r.Body, s.cfg.MaxRequestBodySize)
		if err != nil {
			writeErrorJSON(w, "request body too large", http.StatusRequestEntityTooLarge)
			return
		}
		idemKey, idemErr := idempotencyKeyFromRequest(r)
		if idemErr != nil {
			writeErrorJSON(w, idemErr.Error(), http.StatusBadRequest)
			return
		}
		s.relayAuthorizedAgentHTTP(w, r, p, agentID, deploymentID, r.Method, "/api/v1/session/new", r.URL.RawQuery, body, idemKey)
	default:
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
	}
}

func (s *Server) handleAgentSessionAlias(w http.ResponseWriter, r *http.Request, p *Principal, agentID, deploymentID, sessionID string) {
	switch r.Method {
	case "GET", "DELETE":
		idemKey := ""
		if r.Method != "GET" {
			var idemErr error
			idemKey, idemErr = idempotencyKeyFromRequest(r)
			if idemErr != nil {
				writeErrorJSON(w, idemErr.Error(), http.StatusBadRequest)
				return
			}
		}
		s.relayAuthorizedAgentHTTP(w, r, p, agentID, deploymentID, r.Method, "/api/v1/session/"+sessionID, r.URL.RawQuery, nil, idemKey)
	default:
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
	}
}

func (s *Server) handleAgentSessionAttachAlias(w http.ResponseWriter, r *http.Request, p *Principal, agentID, deploymentID, sessionID string) {
	if r.Method != "POST" {
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	body, err := readBodyBounded(r.Body, s.cfg.MaxRequestBodySize)
	if err != nil {
		writeErrorJSON(w, "request body too large", http.StatusRequestEntityTooLarge)
		return
	}
	patchedBody, patchErr := injectSessionIDIntoAttachBody(body, sessionID)
	if patchErr != nil {
		writeErrorJSON(w, patchErr.Error(), http.StatusBadRequest, "invalid_json")
		return
	}
	idemKey, idemErr := idempotencyKeyFromRequest(r)
	if idemErr != nil {
		writeErrorJSON(w, idemErr.Error(), http.StatusBadRequest)
		return
	}
	s.relayAuthorizedAgentHTTP(w, r, p, agentID, deploymentID, r.Method, "/api/v1/session/attach", r.URL.RawQuery, patchedBody, idemKey)
}

func (s *Server) handleAgentSessionAttachmentAlias(w http.ResponseWriter, r *http.Request, p *Principal, agentID, deploymentID, sessionID, agentPath string) {
	if r.Method != "POST" {
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	body, err := readBodyBounded(r.Body, s.cfg.MaxRequestBodySize)
	if err != nil {
		writeErrorJSON(w, "request body too large", http.StatusRequestEntityTooLarge)
		return
	}
	idemKey, idemErr := idempotencyKeyFromRequest(r)
	if idemErr != nil {
		writeErrorJSON(w, idemErr.Error(), http.StatusBadRequest)
		return
	}
	s.relayAuthorizedAgentHTTP(w, r, p, agentID, deploymentID, r.Method, agentPath, r.URL.RawQuery, body, idemKey)
}

func (s *Server) handleAgentSessionReadAlias(w http.ResponseWriter, r *http.Request, p *Principal, agentID, deploymentID, agentPath string) {
	if r.Method != "GET" {
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	s.relayAuthorizedAgentHTTP(w, r, p, agentID, deploymentID, r.Method, agentPath, r.URL.RawQuery, nil, "")
}

func (s *Server) handleAgentSessionEventsAlias(w http.ResponseWriter, r *http.Request, agentID, agentPath string) {
	s.handleAgentProxySSE(w, r, agentID, agentPath)
}

func (s *Server) handleAgentSessionTranscriptAlias(w http.ResponseWriter, r *http.Request, p *Principal, agentID, deploymentID, sessionID string) {
	s.handleAgentSessionReadAlias(w, r, p, agentID, deploymentID, path.Join("/api/v1/session", sessionID, "transcript"))
}

func (s *Server) handleAgentSessionShellsAlias(w http.ResponseWriter, r *http.Request, p *Principal, agentID, deploymentID, sessionID string, rest []string) {
	basePath := path.Join("/api/v1/session", sessionID, "shells")
	if len(rest) == 0 {
		switch r.Method {
		case "GET":
			s.relayAuthorizedAgentHTTP(w, r, p, agentID, deploymentID, r.Method, basePath, r.URL.RawQuery, nil, "")
		case "POST":
			body, ok := readAliasMutationBody(w, r, s.cfg.MaxRequestBodySize)
			if !ok {
				return
			}
			idemKey, ok := readAliasIdempotencyKey(w, r)
			if !ok {
				return
			}
			s.relayAuthorizedAgentHTTP(w, r, p, agentID, deploymentID, r.Method, path.Join(basePath, "start"), r.URL.RawQuery, body, idemKey)
		default:
			writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
		}
		return
	}
	jobRef, ok := decodeAliasPathSegment(w, rest[0], "invalid job_ref")
	if !ok {
		return
	}
	jobPath := path.Join(basePath, jobRef)
	if len(rest) == 1 {
		s.handleAgentSessionReadAlias(w, r, p, agentID, deploymentID, jobPath)
		return
	}
	if len(rest) != 2 {
		writeErrorJSON(w, "not found", http.StatusNotFound)
		return
	}
	switch rest[1] {
	case "poll", "send", "terminate":
		body, ok := readAliasMutationBody(w, r, s.cfg.MaxRequestBodySize)
		if !ok {
			return
		}
		idemKey, ok := readAliasIdempotencyKey(w, r)
		if !ok {
			return
		}
		s.relayAuthorizedAgentHTTP(w, r, p, agentID, deploymentID, "POST", path.Join(jobPath, rest[1]), r.URL.RawQuery, body, idemKey)
	default:
		writeErrorJSON(w, "not found", http.StatusNotFound)
	}
}

func (s *Server) handleAgentSessionServicesAlias(w http.ResponseWriter, r *http.Request, p *Principal, agentID, deploymentID, sessionID string, rest []string) {
	basePath := path.Join("/api/v1/session", sessionID, "services")
	if len(rest) == 0 {
		s.handleAgentSessionReadAlias(w, r, p, agentID, deploymentID, basePath)
		return
	}
	jobRef, ok := decodeAliasPathSegment(w, rest[0], "invalid job_ref")
	if !ok {
		return
	}
	jobPath := path.Join(basePath, jobRef)
	if len(rest) == 1 {
		s.handleAgentSessionReadAlias(w, r, p, agentID, deploymentID, jobPath)
		return
	}
	if len(rest) != 2 {
		writeErrorJSON(w, "not found", http.StatusNotFound)
		return
	}
	switch rest[1] {
	case "attach", "wait", "run":
		body, ok := readAliasMutationBody(w, r, s.cfg.MaxRequestBodySize)
		if !ok {
			return
		}
		idemKey, ok := readAliasIdempotencyKey(w, r)
		if !ok {
			return
		}
		s.relayAuthorizedAgentHTTP(w, r, p, agentID, deploymentID, "POST", path.Join(jobPath, rest[1]), r.URL.RawQuery, body, idemKey)
	default:
		writeErrorJSON(w, "not found", http.StatusNotFound)
	}
}

func (s *Server) handleAgentSessionCapabilitiesAlias(w http.ResponseWriter, r *http.Request, p *Principal, agentID, deploymentID, sessionID string, rest []string) {
	basePath := path.Join("/api/v1/session", sessionID, "capabilities")
	if len(rest) == 0 {
		s.handleAgentSessionReadAlias(w, r, p, agentID, deploymentID, basePath)
		return
	}
	if len(rest) != 1 {
		writeErrorJSON(w, "not found", http.StatusNotFound)
		return
	}
	capability, ok := decodeAliasPathSegment(w, rest[0], "invalid capability")
	if !ok {
		return
	}
	s.handleAgentSessionReadAlias(w, r, p, agentID, deploymentID, path.Join(basePath, capability))
}

func readAliasMutationBody(w http.ResponseWriter, r *http.Request, maxSize int64) ([]byte, bool) {
	if r.Method != "POST" {
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
		return nil, false
	}
	body, err := readBodyBounded(r.Body, maxSize)
	if err != nil {
		writeErrorJSON(w, "request body too large", http.StatusRequestEntityTooLarge)
		return nil, false
	}
	return body, true
}

func readAliasIdempotencyKey(w http.ResponseWriter, r *http.Request) (string, bool) {
	idemKey, err := idempotencyKeyFromRequest(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusBadRequest)
		return "", false
	}
	return idemKey, true
}

func decodeAliasPathSegment(w http.ResponseWriter, raw string, errMsg string) (string, bool) {
	decoded, err := url.PathUnescape(raw)
	if err != nil || strings.TrimSpace(decoded) == "" {
		writeErrorJSON(w, errMsg, http.StatusBadRequest)
		return "", false
	}
	return decoded, true
}

func injectSessionIDIntoAttachBody(body []byte, sessionID string) ([]byte, error) {
	payload := map[string]any{}
	if len(body) > 0 {
		if err := json.Unmarshal(body, &payload); err != nil {
			return nil, err
		}
	}
	payload["session_id"] = sessionID
	return json.Marshal(payload)
}
