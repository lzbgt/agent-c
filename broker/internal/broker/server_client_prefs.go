package broker

import (
	"encoding/json"
	"net/http"
	"strings"
)

const (
	clientPrefsMaxClientIDLen   = 128
	clientPrefsMaxClientKindLen = 64
	clientPrefsMaxBodyBytes     = 64 * 1024
	clientPrefsVersion          = 1
)

func isSafeClientToken(raw string, maxLen int) bool {
	s := strings.TrimSpace(raw)
	if s == "" || len(s) > maxLen {
		return false
	}
	for _, c := range s {
		ok := (c >= 'a' && c <= 'z') ||
			(c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') ||
			c == '-' || c == '_' || c == '.' || c == ':'
		if !ok {
			return false
		}
	}
	return true
}

func parseClientPrefsQuery(r *http.Request) (string, string, string) {
	if r == nil || r.URL == nil {
		return "", "", "missing client tokens"
	}
	q := r.URL.Query()
	clientID := strings.TrimSpace(q.Get("client_id"))
	clientKind := strings.TrimSpace(q.Get("client_kind"))
	if clientKind == "" {
		clientKind = "webui"
	}
	if clientID == "" {
		return "", "", "missing client_id"
	}
	if !isSafeClientToken(clientID, clientPrefsMaxClientIDLen) {
		return "", "", "invalid client_id"
	}
	if !isSafeClientToken(clientKind, clientPrefsMaxClientKindLen) {
		return "", "", "invalid client_kind"
	}
	return clientKind, clientID, ""
}

func (s *Server) handleClientPrefs(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		s.handleClientPrefsGet(w, r)
	case http.MethodPost:
		s.handleClientPrefsPost(w, r)
	default:
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
	}
}

func (s *Server) handleClientPrefsGet(w http.ResponseWriter, r *http.Request) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if p.AuthKind != "oidc" {
		writeErrorJSON(w, "oidc required", http.StatusForbidden)
		return
	}
	clientKind, clientID, errStr := parseClientPrefsQuery(r)
	if errStr != "" {
		writeErrorJSON(w, errStr, http.StatusBadRequest)
		return
	}
	prefs, found, err := s.cfg.DB.GetClientPrefs(r.Context(), p.Sub, clientKind, clientID)
	if err != nil {
		writeErrorJSON(w, "prefs lookup failed", http.StatusInternalServerError)
		return
	}
	resp := map[string]any{
		"ok":          true,
		"found":       found,
		"client_kind": clientKind,
		"client_id":   clientID,
	}
	if found && prefs != nil {
		var prefsObj any
		if err := json.Unmarshal(prefs.PrefsJSON, &prefsObj); err != nil {
			writeErrorJSON(w, "prefs corrupt", http.StatusInternalServerError, "prefs_corrupt")
			return
		}
		resp["version"] = prefs.Version
		resp["updated_utc_ms"] = prefs.UpdatedAt.UnixMilli()
		resp["prefs"] = prefsObj
	}
	writeJSON(w, resp)
}

func (s *Server) handleClientPrefsPost(w http.ResponseWriter, r *http.Request) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if p.AuthKind != "oidc" {
		writeErrorJSON(w, "oidc required", http.StatusForbidden)
		return
	}
	body, err := readBodyBounded(r.Body, clientPrefsMaxBodyBytes)
	if err != nil {
		if strings.Contains(err.Error(), "too large") {
			writeErrorJSON(w, "prefs body too large", http.StatusRequestEntityTooLarge, "prefs_too_large")
			return
		}
		writeErrorJSON(w, "invalid body", http.StatusBadRequest)
		return
	}
	req := struct {
		ClientID   string         `json:"client_id"`
		ClientKind string         `json:"client_kind"`
		Prefs      map[string]any `json:"prefs"`
	}{}
	if err := json.Unmarshal(body, &req); err != nil {
		writeErrorJSON(w, "invalid json", http.StatusBadRequest)
		return
	}
	clientID := strings.TrimSpace(req.ClientID)
	clientKind := strings.TrimSpace(req.ClientKind)
	if clientKind == "" {
		clientKind = "webui"
	}
	if clientID == "" {
		writeErrorJSON(w, "missing client_id", http.StatusBadRequest)
		return
	}
	if !isSafeClientToken(clientID, clientPrefsMaxClientIDLen) {
		writeErrorJSON(w, "invalid client_id", http.StatusBadRequest)
		return
	}
	if !isSafeClientToken(clientKind, clientPrefsMaxClientKindLen) {
		writeErrorJSON(w, "invalid client_kind", http.StatusBadRequest)
		return
	}
	if req.Prefs == nil {
		writeErrorJSON(w, "prefs must be an object", http.StatusBadRequest)
		return
	}
	saved, err := s.cfg.DB.UpsertClientPrefs(r.Context(), p.Sub, clientKind, clientID, clientPrefsVersion, req.Prefs)
	if err != nil {
		writeErrorJSON(w, "prefs update failed", http.StatusInternalServerError)
		return
	}
	resp := map[string]any{
		"ok":          true,
		"found":       true,
		"client_kind": clientKind,
		"client_id":   clientID,
		"version":     saved.Version,
		"updated_utc_ms": func() int64 {
			if saved.UpdatedAt.IsZero() {
				return 0
			}
			return saved.UpdatedAt.UnixMilli()
		}(),
		"prefs": req.Prefs,
	}
	writeJSON(w, resp)
}
