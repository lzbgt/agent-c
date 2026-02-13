package broker

import (
	"net/http"
	"strings"
)

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
