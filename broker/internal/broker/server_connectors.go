package broker

import (
	"encoding/json"
	"net/http"
	"strings"
	"time"
)

func (s *Server) handleConnectors(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	_, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	connectors := []any{}
	count := 0
	if s.cfg.Connectors != nil {
		list := s.cfg.Connectors.List()
		count = len(list)
		connectors = make([]any, 0, len(list))
		for _, c := range list {
			connectors = append(connectors, c)
		}
	}
	writeJSON(w, map[string]any{
		"ok":         true,
		"count":      count,
		"connectors": connectors,
	})
}

func (s *Server) handleConnectorsSubroutes(w http.ResponseWriter, r *http.Request) {
	path := strings.TrimPrefix(r.URL.Path, "/v1/connectors/")
	path = strings.Trim(path, "/")
	if path == "" {
		writeErrorJSON(w, "connector id required", http.StatusBadRequest)
		return
	}
	parts := strings.Split(path, "/")
	connectorID := strings.TrimSpace(parts[0])
	action := ""
	if len(parts) > 1 {
		action = parts[1]
	}
	if action != "status" {
		writeErrorJSON(w, "not found", http.StatusNotFound)
		return
	}
	if r.Method != http.MethodPost {
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if !p.Admin {
		writeErrorJSON(w, "admin required", http.StatusForbidden)
		return
	}
	var req struct {
		Status    string `json:"status"`
		LastError string `json:"last_error"`
		TsUnixMs  int64  `json:"ts_unix_ms"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeErrorJSON(w, "invalid json body", http.StatusBadRequest)
		return
	}
	if s.cfg.Connectors == nil {
		writeErrorJSON(w, "connector registry disabled", http.StatusNotFound)
		return
	}
	ts := req.TsUnixMs
	if ts <= 0 {
		ts = time.Now().UnixMilli()
	}
	updated, ok := s.cfg.Connectors.UpdateStatus(connectorID, req.Status, req.LastError, ts)
	if !ok {
		writeErrorJSON(w, "connector not found", http.StatusNotFound)
		return
	}
	writeJSON(w, map[string]any{
		"ok":        true,
		"connector": updated,
	})
}
