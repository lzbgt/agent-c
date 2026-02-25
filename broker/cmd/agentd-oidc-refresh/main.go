package main

import (
	"context"
	"crypto/tls"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

type config struct {
	issuer        string
	keycloakBase  string
	realm         string
	tokenURL      string
	clientID      string
	clientSecret  string
	username      string
	password      string
	refreshToken  string
	scope         string
	output        string
	refreshBefore time.Duration
	minInterval   time.Duration
	once          bool
	insecureTLS   bool
	printToken    bool
}

type tokenResponse struct {
	AccessToken  string `json:"access_token"`
	ExpiresIn    int64  `json:"expires_in"`
	RefreshToken string `json:"refresh_token"`
}

type oidcConfig struct {
	TokenEndpoint string `json:"token_endpoint"`
}

func main() {
	cfg := parseFlags()
	if err := cfg.validate(); err != nil {
		fmt.Fprintf(os.Stderr, "config error: %v\n", err)
		os.Exit(2)
	}
	ctx := context.Background()
	client := httpClient(cfg.insecureTLS)

	for {
		resp, err := fetchToken(ctx, client, &cfg)
		if err != nil {
			fmt.Fprintf(os.Stderr, "token refresh error: %v\n", err)
			if cfg.once {
				os.Exit(1)
			}
			sleep(cfg.minInterval)
			continue
		}
		if resp.RefreshToken != "" {
			cfg.refreshToken = resp.RefreshToken
		}
		if cfg.output != "" {
			if err := writeTokenFile(cfg.output, resp.AccessToken); err != nil {
				fmt.Fprintf(os.Stderr, "token write error: %v\n", err)
				if cfg.once {
					os.Exit(1)
				}
			}
		}
		if cfg.printToken {
			fmt.Println(resp.AccessToken)
		}
		if cfg.once {
			return
		}

		sleepFor := 5 * time.Minute
		if resp.ExpiresIn > 0 {
			exp := time.Duration(resp.ExpiresIn) * time.Second
			sleepFor = exp - cfg.refreshBefore
		}
		if sleepFor < cfg.minInterval {
			sleepFor = cfg.minInterval
		}
		sleep(sleepFor)
	}
}

func parseFlags() config {
	cfg := config{}
	flag.StringVar(&cfg.issuer, "issuer", strings.TrimSpace(os.Getenv("OIDC_ISSUER")), "OIDC issuer URL (env: OIDC_ISSUER).")
	flag.StringVar(&cfg.keycloakBase, "keycloak-base", strings.TrimSpace(os.Getenv("OIDC_KEYCLOAK_BASE")), "Keycloak base URL (env: OIDC_KEYCLOAK_BASE).")
	flag.StringVar(&cfg.realm, "realm", envDefault("OIDC_REALM", "agentd"), "Keycloak realm (env: OIDC_REALM).")
	flag.StringVar(&cfg.tokenURL, "token-url", strings.TrimSpace(os.Getenv("OIDC_TOKEN_URL")), "Explicit token endpoint URL (env: OIDC_TOKEN_URL).")
	flag.StringVar(&cfg.clientID, "client-id", strings.TrimSpace(os.Getenv("OIDC_CLIENT_ID")), "OIDC client id (env: OIDC_CLIENT_ID).")
	flag.StringVar(&cfg.clientSecret, "client-secret", strings.TrimSpace(os.Getenv("OIDC_CLIENT_SECRET")), "OIDC client secret (env: OIDC_CLIENT_SECRET).")
	flag.StringVar(&cfg.username, "user", strings.TrimSpace(os.Getenv("OIDC_USERNAME")), "OIDC username (env: OIDC_USERNAME).")
	flag.StringVar(&cfg.password, "password", strings.TrimSpace(os.Getenv("OIDC_PASSWORD")), "OIDC password (env: OIDC_PASSWORD).")
	flag.StringVar(&cfg.refreshToken, "refresh-token", strings.TrimSpace(os.Getenv("OIDC_REFRESH_TOKEN")), "OIDC refresh token (env: OIDC_REFRESH_TOKEN).")
	flag.StringVar(&cfg.scope, "scope", strings.TrimSpace(os.Getenv("OIDC_SCOPE")), "OIDC scope (env: OIDC_SCOPE).")
	flag.StringVar(&cfg.output, "output", strings.TrimSpace(os.Getenv("OIDC_TOKEN_FILE")), "Write access token to file (env: OIDC_TOKEN_FILE).")
	flag.DurationVar(&cfg.refreshBefore, "refresh-before", envDurationSeconds("OIDC_REFRESH_BEFORE", 60), "Refresh N seconds before expiry (env: OIDC_REFRESH_BEFORE).")
	flag.DurationVar(&cfg.minInterval, "min-interval", envDurationSeconds("OIDC_MIN_INTERVAL", 20), "Minimum sleep between refreshes (env: OIDC_MIN_INTERVAL).")
	flag.BoolVar(&cfg.once, "once", strings.TrimSpace(os.Getenv("OIDC_ONCE")) == "1", "Fetch token once and exit (env: OIDC_ONCE=1).")
	flag.BoolVar(&cfg.insecureTLS, "insecure", strings.TrimSpace(os.Getenv("OIDC_INSECURE_TLS")) == "1", "Skip TLS verification (env: OIDC_INSECURE_TLS=1).")
	flag.BoolVar(&cfg.printToken, "print", strings.TrimSpace(os.Getenv("OIDC_PRINT_TOKEN")) == "1", "Print access token to stdout (env: OIDC_PRINT_TOKEN=1).")
	flag.Parse()
	return cfg
}

func (c *config) validate() error {
	if c.clientID == "" {
		return fmt.Errorf("missing client id")
	}
	if c.refreshToken == "" && (c.username == "" || c.password == "") && c.clientSecret == "" {
		return fmt.Errorf("missing grant parameters (refresh token, username/password, or client secret)")
	}
	if c.refreshBefore <= 0 {
		c.refreshBefore = 60 * time.Second
	}
	if c.minInterval <= 0 {
		c.minInterval = 20 * time.Second
	}
	if c.output != "" {
		if err := ensureDir(c.output); err != nil {
			return err
		}
	}
	return nil
}

func resolveTokenURL(ctx context.Context, client *http.Client, cfg *config) (string, error) {
	if cfg.tokenURL != "" {
		return cfg.tokenURL, nil
	}
	if cfg.issuer != "" {
		endpoint := strings.TrimRight(cfg.issuer, "/") + "/.well-known/openid-configuration"
		req, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint, nil)
		if err != nil {
			return "", err
		}
		resp, err := client.Do(req)
		if err != nil {
			return "", err
		}
		defer resp.Body.Close()
		if resp.StatusCode >= 300 {
			return "", fmt.Errorf("issuer discovery failed: http %d", resp.StatusCode)
		}
		body, err := io.ReadAll(resp.Body)
		if err != nil {
			return "", err
		}
		var cfgResp oidcConfig
		if err := json.Unmarshal(body, &cfgResp); err != nil {
			return "", err
		}
		if cfgResp.TokenEndpoint == "" {
			return "", fmt.Errorf("issuer discovery missing token_endpoint")
		}
		return cfgResp.TokenEndpoint, nil
	}
	if cfg.keycloakBase != "" {
		base := strings.TrimRight(cfg.keycloakBase, "/")
		realm := strings.Trim(cfg.realm, "/")
		if realm == "" {
			realm = "agentd"
		}
		return fmt.Sprintf("%s/realms/%s/protocol/openid-connect/token", base, realm), nil
	}
	return "", fmt.Errorf("missing token endpoint (set --token-url, --issuer, or --keycloak-base)")
}

func fetchToken(ctx context.Context, client *http.Client, cfg *config) (*tokenResponse, error) {
	urlStr, err := resolveTokenURL(ctx, client, cfg)
	if err != nil {
		return nil, err
	}
	values := url.Values{}
	grant := ""
	if cfg.refreshToken != "" {
		grant = "refresh_token"
		values.Set("refresh_token", cfg.refreshToken)
	} else if cfg.username != "" && cfg.password != "" {
		grant = "password"
		values.Set("username", cfg.username)
		values.Set("password", cfg.password)
	} else {
		grant = "client_credentials"
	}
	values.Set("grant_type", grant)
	values.Set("client_id", cfg.clientID)
	if cfg.clientSecret != "" {
		values.Set("client_secret", cfg.clientSecret)
	}
	if cfg.scope != "" {
		values.Set("scope", cfg.scope)
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodPost, urlStr, strings.NewReader(values.Encode()))
	if err != nil {
		return nil, err
	}
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")

	resp, err := client.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, err
	}
	if resp.StatusCode >= 300 {
		return nil, fmt.Errorf("token endpoint error: http %d: %s", resp.StatusCode, strings.TrimSpace(string(data)))
	}
	var tr tokenResponse
	if err := json.Unmarshal(data, &tr); err != nil {
		return nil, err
	}
	if tr.AccessToken == "" {
		return nil, errors.New("missing access_token in response")
	}
	return &tr, nil
}

func writeTokenFile(path, token string) error {
	if token == "" {
		return errors.New("empty token")
	}
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, []byte(token), 0o600); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

func ensureDir(path string) error {
	dir := filepath.Dir(path)
	if dir == "." || dir == "" {
		return nil
	}
	return os.MkdirAll(dir, 0o700)
}

func envDefault(key, fallback string) string {
	if v := strings.TrimSpace(os.Getenv(key)); v != "" {
		return v
	}
	return fallback
}

func envDurationSeconds(key string, fallback int) time.Duration {
	v := strings.TrimSpace(os.Getenv(key))
	if v == "" {
		return time.Duration(fallback) * time.Second
	}
	if n, err := strconv.Atoi(v); err == nil {
		return time.Duration(n) * time.Second
	}
	return time.Duration(fallback) * time.Second
}

func sleep(d time.Duration) {
	if d <= 0 {
		return
	}
	t := time.NewTimer(d)
	<-t.C
}

func httpClient(insecure bool) *http.Client {
	transport := http.DefaultTransport.(*http.Transport).Clone()
	if insecure {
		transport.TLSClientConfig = &tls.Config{InsecureSkipVerify: true}
	}
	return &http.Client{Timeout: 30 * time.Second, Transport: transport}
}
