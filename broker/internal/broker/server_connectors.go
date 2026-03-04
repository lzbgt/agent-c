package broker

import (
	"net/http"
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
