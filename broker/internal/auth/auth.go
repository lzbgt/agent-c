package auth

import (
	"crypto/tls"
	"crypto/x509"
	"errors"
	"fmt"
	"net/http"
	"os"
	"strings"
)

type ClientPolicy struct {
	ClientID      string
	Token         string
	Admin         bool
	AllowedAgents map[string]bool
}

type ClientAuth struct {
	ByToken map[string]*ClientPolicy
}

func (a *ClientAuth) AuthenticateBearer(r *http.Request) (*ClientPolicy, error) {
	if a == nil || len(a.ByToken) == 0 {
		return nil, errors.New("client auth not configured")
	}
	if r == nil {
		return nil, errors.New("nil request")
	}
	raw := strings.TrimSpace(r.Header.Get("Authorization"))
	if raw == "" {
		return nil, errors.New("missing Authorization header")
	}
	const pfx = "Bearer "
	if !strings.HasPrefix(raw, pfx) {
		return nil, errors.New("expected Authorization: Bearer <token>")
	}
	tok := strings.TrimSpace(strings.TrimPrefix(raw, pfx))
	if tok == "" {
		return nil, errors.New("empty bearer token")
	}
	p := a.ByToken[tok]
	if p == nil {
		return nil, errors.New("invalid token")
	}
	return p, nil
}

func (p *ClientPolicy) CanAccessAgent(agentID string) bool {
	if p == nil {
		return false
	}
	if p.Admin {
		return true
	}
	if p.AllowedAgents == nil {
		return false
	}
	return p.AllowedAgents[agentID]
}

// VerifiedClientLeaf returns the verified client certificate leaf from a request TLS state.
// Requires that Go's TLS stack verified the chain (VerifiedChains is non-empty).
func VerifiedClientLeaf(r *http.Request) (*x509.Certificate, error) {
	if r == nil {
		return nil, errors.New("nil request")
	}
	cs := r.TLS
	if cs == nil {
		return nil, errors.New("missing TLS connection state")
	}
	if len(cs.VerifiedChains) == 0 || len(cs.VerifiedChains[0]) == 0 {
		return nil, errors.New("missing verified client certificate")
	}
	return cs.VerifiedChains[0][0], nil
}

// AgentIDFromCertCN extracts an agent id from a verified certificate CN, using a CN prefix convention.
// Example: AllowedCNPfx="agentd-" and CN="agentd-123" => "123".
func AgentIDFromCertCN(cert *x509.Certificate, allowedCNPfx string) (string, error) {
	if cert == nil {
		return "", errors.New("nil cert")
	}
	pfx := strings.TrimSpace(allowedCNPfx)
	if pfx == "" {
		pfx = "agentd-"
	}
	cn := strings.TrimSpace(cert.Subject.CommonName)
	if cn == "" {
		return "", errors.New("client cert CN empty")
	}
	if !strings.HasPrefix(strings.ToLower(cn), strings.ToLower(pfx)) {
		return "", fmt.Errorf("client cert CN %q does not start with %q", cn, pfx)
	}
	id := strings.TrimSpace(cn[len(pfx):])
	if id == "" {
		return "", fmt.Errorf("client cert CN %q missing id suffix after %q", cn, pfx)
	}
	return id, nil
}

func ConfigureServerTLS(tlsCert, tlsKey, tlsClientCA string) (*tls.Config, error) {
	cfg := &tls.Config{
		MinVersion: tls.VersionTLS12,
	}
	if strings.TrimSpace(tlsClientCA) != "" {
		b, err := osReadFile(tlsClientCA)
		if err != nil {
			return nil, err
		}
		pool := x509.NewCertPool()
		if !pool.AppendCertsFromPEM(b) {
			return nil, fmt.Errorf("failed to parse tls client CA PEM: %s", tlsClientCA)
		}
		cfg.ClientCAs = pool
		// Keep browser clients usable. We require agent mTLS at the handler level.
		cfg.ClientAuth = tls.VerifyClientCertIfGiven
	}
	_ = tlsCert
	_ = tlsKey
	return cfg, nil
}

// indirection for tests
var osReadFile = func(path string) ([]byte, error) { return os.ReadFile(path) }
