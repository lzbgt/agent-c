package broker

import (
	"net/http"
	"strings"
)

func (s *Server) handleAuthSession(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case "POST":
		s.handleAuthSessionCreate(w, r)
	case "DELETE":
		s.handleAuthSessionDelete(w, r)
	default:
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
	}
}

func (s *Server) authCookieName() string {
	if s == nil {
		return ""
	}
	return strings.TrimSpace(s.cfg.AuthCookieName)
}

func (s *Server) handleAuthSessionCreate(w http.ResponseWriter, r *http.Request) {
	cookieName := s.authCookieName()
	if cookieName == "" {
		writeErrorJSON(w, "auth cookie not configured", http.StatusBadRequest)
		return
	}
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	token := authTokenFromAuthorizationHeader(r)
	if token == "" {
		token = authTokenFromCookie(r, cookieName)
	}
	if token == "" {
		writeErrorJSON(w, "missing bearer token", http.StatusUnauthorized)
		return
	}
	secure := requestIsHTTPS(r)
	sameSite := authCookieSameSite(secure)
	http.SetCookie(w, &http.Cookie{
		Name:     cookieName,
		Value:    token,
		Path:     "/",
		HttpOnly: true,
		Secure:   secure,
		SameSite: sameSite,
	})
	writeJSON(w, map[string]any{
		"ok":          true,
		"cookie_name": cookieName,
		"auth_kind":   p.AuthKind,
		"http_only":   true,
		"secure":      secure,
		"same_site":   sameSiteLabel(sameSite),
	})
}

func (s *Server) handleAuthSessionDelete(w http.ResponseWriter, r *http.Request) {
	cookieName := s.authCookieName()
	if cookieName == "" {
		writeErrorJSON(w, "auth cookie not configured", http.StatusBadRequest)
		return
	}
	secure := requestIsHTTPS(r)
	sameSite := authCookieSameSite(secure)
	http.SetCookie(w, &http.Cookie{
		Name:     cookieName,
		Value:    "",
		Path:     "/",
		HttpOnly: true,
		Secure:   secure,
		SameSite: sameSite,
		MaxAge:   -1,
	})
	writeJSON(w, map[string]any{
		"ok":          true,
		"cleared":     true,
		"cookie_name": cookieName,
		"http_only":   true,
		"secure":      secure,
		"same_site":   sameSiteLabel(sameSite),
	})
}

func sameSiteLabel(mode http.SameSite) string {
	switch mode {
	case http.SameSiteNoneMode:
		return "none"
	case http.SameSiteLaxMode:
		return "lax"
	case http.SameSiteStrictMode:
		return "strict"
	default:
		return "default"
	}
}
