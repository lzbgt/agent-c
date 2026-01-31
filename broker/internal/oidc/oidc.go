package oidc

import (
	"context"
	"errors"
	"net/http"
	"strings"
	"sync"
	"time"

	gooidc "github.com/coreos/go-oidc/v3/oidc"
)

// Verifier validates OIDC JWTs and exposes the subject claim.
//
// This is intentionally minimal:
// - verify signature via issuer JWKS
// - verify issuer + audience
// - extract sub
//
// Extension points (future):
// - extract groups/roles
// - multi-tenant audience routing
type Verifier struct {
	IssuerURL string
	Audience  string

	mu              sync.Mutex
	provider        *gooidc.Provider
	verifier        *gooidc.IDTokenVerifier
	lastInitAttempt time.Time
	lastInitErr     error
}

func (v *Verifier) Init(ctx context.Context) error {
	v.mu.Lock()
	defer v.mu.Unlock()
	if v.verifier != nil {
		return nil
	}
	// Retry with a small backoff to tolerate transient startup ordering (e.g. Keycloak not ready yet).
	// Without this, a single transient failure would permanently poison the verifier instance.
	if !v.lastInitAttempt.IsZero() && time.Since(v.lastInitAttempt) < 5*time.Second && v.lastInitErr != nil {
		return v.lastInitErr
	}
	v.lastInitAttempt = time.Now()

	iss := strings.TrimSpace(v.IssuerURL)
	if iss == "" {
		v.lastInitErr = errors.New("missing oidc issuer url")
		return v.lastInitErr
	}
	if strings.TrimSpace(v.Audience) == "" {
		v.lastInitErr = errors.New("missing oidc audience")
		return v.lastInitErr
	}

	prov, err := gooidc.NewProvider(ctx, iss)
	if err != nil {
		v.lastInitErr = err
		return v.lastInitErr
	}
	v.provider = prov

	cfg := &gooidc.Config{
		// We validate audience ourselves because different IdPs (notably Keycloak)
		// often put the client id in `azp` for access tokens, while `aud` may be a
		// generic resource like "account".
		//
		// Security notes:
		// - signature verification is still done via the issuer JWKS
		// - issuer is still verified by go-oidc
		// - we enforce aud/azp below (AuthenticateRequest)
		SkipClientIDCheck: true,
	}
	v.verifier = prov.Verifier(cfg)
	v.lastInitErr = nil
	return nil
}

type Principal struct {
	Sub string
}

func (v *Verifier) AuthenticateRequest(ctx context.Context, r *http.Request) (*Principal, error) {
	if r == nil {
		return nil, errors.New("nil request")
	}
	if err := v.Init(ctx); err != nil {
		return nil, err
	}

	raw := strings.TrimSpace(r.Header.Get("Authorization"))
	if raw == "" {
		return nil, errors.New("missing Authorization header")
	}
	const pfx = "Bearer "
	if !strings.HasPrefix(raw, pfx) {
		return nil, errors.New("expected Authorization: Bearer <jwt>")
	}
	jwt := strings.TrimSpace(strings.TrimPrefix(raw, pfx))
	if jwt == "" {
		return nil, errors.New("empty bearer token")
	}

	// Bound verification time in case of a hung context.
	vctx, cancel := context.WithTimeout(ctx, 8*time.Second)
	defer cancel()

	tok, err := v.verifier.Verify(vctx, jwt)
	if err != nil {
		return nil, err
	}

	claims := struct {
		Sub string `json:"sub"`
		Azp string `json:"azp"`
		Aud any    `json:"aud"`
	}{}
	if err := tok.Claims(&claims); err != nil {
		return nil, err
	}
	if strings.TrimSpace(claims.Sub) == "" {
		return nil, errors.New("missing sub claim")
	}

	wantAud := strings.TrimSpace(v.Audience)
	if wantAud == "" {
		return nil, errors.New("missing configured audience")
	}
	// Accept:
	// - aud == wantAud
	// - aud contains wantAud
	// - azp == wantAud (common for Keycloak access tokens)
	if strings.TrimSpace(claims.Azp) == wantAud {
		return &Principal{Sub: claims.Sub}, nil
	}
	switch av := claims.Aud.(type) {
	case string:
		if strings.TrimSpace(av) == wantAud {
			return &Principal{Sub: claims.Sub}, nil
		}
	case []any:
		for _, it := range av {
			if s, ok := it.(string); ok && strings.TrimSpace(s) == wantAud {
				return &Principal{Sub: claims.Sub}, nil
			}
		}
	case []string:
		for _, s := range av {
			if strings.TrimSpace(s) == wantAud {
				return &Principal{Sub: claims.Sub}, nil
			}
		}
	default:
		// ignore; fallthrough
	}
	return nil, errors.New("token audience mismatch")
}
