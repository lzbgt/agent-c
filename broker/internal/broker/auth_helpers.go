package broker

import (
	"net/http"
	"strings"
)

func authTokenFromAuthorizationHeader(r *http.Request) string {
	if r == nil {
		return ""
	}
	raw := strings.TrimSpace(r.Header.Get("Authorization"))
	if raw == "" {
		return ""
	}
	if strings.HasPrefix(strings.ToLower(raw), "bearer ") {
		return strings.TrimSpace(raw[7:])
	}
	return strings.TrimSpace(raw)
}

func authTokenFromCookie(r *http.Request, name string) string {
	if r == nil {
		return ""
	}
	name = strings.TrimSpace(name)
	if name == "" {
		return ""
	}
	c, err := r.Cookie(name)
	if err != nil || c == nil {
		return ""
	}
	tok := strings.TrimSpace(c.Value)
	if tok == "" {
		return ""
	}
	if strings.HasPrefix(strings.ToLower(tok), "bearer ") {
		return strings.TrimSpace(tok[7:])
	}
	return tok
}

func requestWithBearer(r *http.Request, token string) *http.Request {
	if r == nil {
		return r
	}
	tok := strings.TrimSpace(token)
	if tok == "" {
		return r
	}
	clone := r.Clone(r.Context())
	clone.Header.Set("Authorization", "Bearer "+tok)
	return clone
}

func requestIsHTTPS(r *http.Request) bool {
	if r == nil {
		return false
	}
	if r.TLS != nil {
		return true
	}
	if v := strings.TrimSpace(r.Header.Get("X-Forwarded-Proto")); strings.EqualFold(v, "https") {
		return true
	}
	if fwd := strings.TrimSpace(r.Header.Get("Forwarded")); fwd != "" {
		for _, entry := range strings.Split(fwd, ",") {
			for _, part := range strings.Split(entry, ";") {
				part = strings.TrimSpace(part)
				if strings.HasPrefix(strings.ToLower(part), "proto=") && strings.EqualFold(strings.Trim(strings.TrimSpace(part[6:]), `"`), "https") {
					return true
				}
			}
		}
	}
	return false
}

func authCookieSameSite(secure bool) http.SameSite {
	if secure {
		return http.SameSiteNoneMode
	}
	return http.SameSiteLaxMode
}
