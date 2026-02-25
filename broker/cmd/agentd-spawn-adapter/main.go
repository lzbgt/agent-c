package main

import (
	"bytes"
	"context"
	"crypto/rand"
	"crypto/tls"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"os/signal"
	"strings"
	"time"
)

type config struct {
	brokerBase     string
	oidcToken      string
	oidcTokenFile  string
	insecureTLS    bool
	pollInterval   time.Duration
	command        string
	commandTimeout time.Duration
	once           bool
	limit          int
	status         string
	adapterID      string
	useAllocator   bool
}

type teamRow struct {
	TeamID string `json:"team_id"`
}

type teamListResponse struct {
	OK    bool      `json:"ok"`
	Teams []teamRow `json:"teams"`
}

type spawnRequest struct {
	SpawnRequestID    string           `json:"spawn_request_id"`
	TeamID            string           `json:"team_id"`
	OrchestratorRunID string           `json:"orchestrator_run_id"`
	Role              string           `json:"role"`
	Count             int              `json:"count"`
	Status            string           `json:"status"`
	Requirements      map[string]any   `json:"requirements"`
	AssignedMembers   []map[string]any `json:"assigned_members"`
	Meta              map[string]any   `json:"meta"`
	Error             string           `json:"error"`
	CreatedUnixMS     int64            `json:"created_unix_ms"`
	UpdatedUnixMS     int64            `json:"updated_unix_ms"`
	CreatedBy         string           `json:"created_by"`
}

type spawnListResponse struct {
	OK            bool           `json:"ok"`
	TeamID        string         `json:"team_id"`
	SpawnRequests []spawnRequest `json:"spawn_requests"`
}

type spawnResponse struct {
	OK           bool         `json:"ok"`
	TeamID       string       `json:"team_id"`
	SpawnRequest spawnRequest `json:"spawn_request"`
}

type spawnCommandOutput struct {
	Status          string           `json:"status"`
	AssignedMembers []map[string]any `json:"assigned_members"`
	Error           string           `json:"error"`
	Meta            map[string]any   `json:"meta"`
}

type runtimeMember struct {
	AgentID      string `json:"agent_id"`
	DeploymentID string `json:"deployment_id"`
	Role         string `json:"role"`
}

type runtimeAllocateResponse struct {
	OK             bool            `json:"ok"`
	TeamID         string          `json:"team_id"`
	RuntimeMembers []runtimeMember `json:"runtime_members"`
	AllocatedRoles []string        `json:"allocated_roles"`
	MissingRoles   []string        `json:"missing_roles"`
	Warnings       []string        `json:"warnings"`
	Error          string          `json:"error"`
	Err            string          `json:"err"`
	Code           string          `json:"code"`
}

type httpError struct {
	Status int
	Body   string
}

func (e *httpError) Error() string {
	return fmt.Sprintf("http %d: %s", e.Status, e.Body)
}

func main() {
	cfg := parseFlags()
	if err := cfg.validate(); err != nil {
		fmt.Fprintf(os.Stderr, "config error: %v\n", err)
		os.Exit(2)
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt)
	defer stop()

	client := httpClient(cfg.insecureTLS)
	for {
		if err := runOnce(ctx, client, cfg); err != nil {
			fmt.Fprintf(os.Stderr, "spawn adapter error: %v\n", err)
		}
		if cfg.once {
			return
		}
		select {
		case <-ctx.Done():
			return
		case <-time.After(cfg.pollInterval):
		}
	}
}

func parseFlags() config {
	var cfg config
	flag.StringVar(&cfg.brokerBase, "broker-base", strings.TrimSpace(os.Getenv("BROKER_BASE")), "Broker base URL (env: BROKER_BASE).")
	flag.StringVar(&cfg.oidcToken, "oidc-token", strings.TrimSpace(os.Getenv("BROKER_OIDC_TOKEN")), "Bearer token for broker auth (env: BROKER_OIDC_TOKEN).")
	flag.StringVar(&cfg.oidcTokenFile, "oidc-token-file", strings.TrimSpace(os.Getenv("BROKER_OIDC_TOKEN_FILE")), "Path to broker auth token file (env: BROKER_OIDC_TOKEN_FILE).")
	flag.BoolVar(&cfg.insecureTLS, "insecure", strings.TrimSpace(os.Getenv("BROKER_INSECURE_TLS")) == "1", "Skip TLS verification (env: BROKER_INSECURE_TLS=1).")
	flag.DurationVar(&cfg.pollInterval, "poll-interval", 3*time.Second, "Polling interval when not --once.")
	flag.StringVar(&cfg.command, "command", strings.TrimSpace(os.Getenv("SPAWN_COMMAND")), "Shell command to fulfill a spawn request (env: SPAWN_COMMAND).")
	flag.DurationVar(&cfg.commandTimeout, "command-timeout", 2*time.Minute, "Timeout for the spawn command execution.")
	flag.BoolVar(&cfg.once, "once", false, "Process one poll cycle and exit.")
	flag.IntVar(&cfg.limit, "limit", 50, "Max spawn requests to fetch per team per poll.")
	flag.StringVar(&cfg.status, "status", "requested", "Spawn request status to filter (default: requested).")
	flag.StringVar(&cfg.adapterID, "adapter-id", strings.TrimSpace(os.Getenv("SPAWN_ADAPTER_ID")), "Adapter id stored in spawn request meta.")
	flag.BoolVar(&cfg.useAllocator, "allocator", envBool("SPAWN_ALLOCATOR"), "Use broker runtime-member allocator instead of SPAWN_COMMAND (env: SPAWN_ALLOCATOR=1).")
	flag.Parse()
	if cfg.adapterID == "" {
		cfg.adapterID = "adapter_" + randID(8)
	}
	return cfg
}

func (c config) validate() error {
	if c.brokerBase == "" {
		return fmt.Errorf("missing broker base url")
	}
	if c.oidcToken == "" && c.oidcTokenFile == "" {
		return fmt.Errorf("missing oidc token")
	}
	if !c.useAllocator && c.command == "" {
		return fmt.Errorf("missing spawn command")
	}
	if c.limit <= 0 {
		c.limit = 50
	}
	if c.pollInterval <= 0 {
		c.pollInterval = 3 * time.Second
	}
	if c.commandTimeout <= 0 {
		c.commandTimeout = 2 * time.Minute
	}
	if c.status == "" {
		c.status = "requested"
	}
	return nil
}

func (c config) bearerToken() (string, error) {
	if c.oidcTokenFile != "" {
		if data, err := os.ReadFile(c.oidcTokenFile); err == nil {
			token := strings.TrimSpace(string(data))
			if token != "" {
				return token, nil
			}
		} else if strings.TrimSpace(c.oidcToken) == "" {
			return "", fmt.Errorf("read oidc token file: %w", err)
		}
	}
	if strings.TrimSpace(c.oidcToken) == "" {
		return "", fmt.Errorf("missing oidc token")
	}
	return strings.TrimSpace(c.oidcToken), nil
}

func runOnce(ctx context.Context, client *http.Client, cfg config) error {
	teams, err := fetchTeams(ctx, client, cfg)
	if err != nil {
		return err
	}
	for _, team := range teams {
		if strings.TrimSpace(team.TeamID) == "" {
			continue
		}
		spawnRequests, err := fetchSpawnRequests(ctx, client, cfg, team.TeamID)
		if err != nil {
			return err
		}
		for _, req := range spawnRequests {
			if strings.TrimSpace(req.SpawnRequestID) == "" {
				continue
			}
			if strings.TrimSpace(req.Status) != cfg.status {
				continue
			}
			if err := handleSpawnRequest(ctx, client, cfg, req); err != nil {
				fmt.Fprintf(os.Stderr, "spawn request %s error: %v\n", req.SpawnRequestID, err)
			}
		}
	}
	return nil
}

func handleSpawnRequest(ctx context.Context, client *http.Client, cfg config, req spawnRequest) error {
	claimedMeta := cloneMap(req.Meta)
	now := time.Now().UnixMilli()
	claimedMeta["adapter_id"] = cfg.adapterID
	claimedMeta["adapter_claimed_unix_ms"] = now
	if _, err := updateSpawnRequest(ctx, client, cfg, req.TeamID, req.SpawnRequestID, map[string]any{
		"status":          "allocating",
		"expected_status": req.Status,
		"meta":            claimedMeta,
	}); err != nil {
		if isHTTPStatus(err, http.StatusConflict) {
			return nil
		}
		return err
	}
	if cfg.useAllocator {
		return handleSpawnRequestAllocator(ctx, client, cfg, req, claimedMeta)
	}
	out, err := runCommand(ctx, cfg, req, claimedMeta)
	if err != nil {
		_, patchErr := updateSpawnRequest(ctx, client, cfg, req.TeamID, req.SpawnRequestID, map[string]any{
			"status": "error",
			"error":  err.Error(),
			"meta":   claimedMeta,
		})
		if patchErr != nil {
			return fmt.Errorf("spawn command failed: %v (patch error: %w)", err, patchErr)
		}
		return err
	}
	meta := cloneMap(claimedMeta)
	if len(out.Meta) > 0 {
		for k, v := range out.Meta {
			meta[k] = v
		}
	}
	status := strings.TrimSpace(out.Status)
	if status == "" {
		status = "allocated"
	}
	if strings.TrimSpace(out.Error) != "" {
		status = "error"
	}
	patch := map[string]any{
		"status": status,
		"meta":   meta,
	}
	if out.AssignedMembers != nil {
		patch["assigned_members"] = out.AssignedMembers
	}
	if strings.TrimSpace(out.Error) != "" {
		patch["error"] = out.Error
	}
	if _, err := updateSpawnRequest(ctx, client, cfg, req.TeamID, req.SpawnRequestID, patch); err != nil {
		return err
	}
	return nil
}

func handleSpawnRequestAllocator(
	ctx context.Context,
	client *http.Client,
	cfg config,
	req spawnRequest,
	claimedMeta map[string]any,
) error {
	role := strings.TrimSpace(req.Role)
	if role == "" {
		_, patchErr := updateSpawnRequest(ctx, client, cfg, req.TeamID, req.SpawnRequestID, map[string]any{
			"status": "error",
			"error":  "missing role",
			"meta":   claimedMeta,
		})
		if patchErr != nil {
			return fmt.Errorf("allocator failed: missing role (patch error: %w)", patchErr)
		}
		return fmt.Errorf("allocator failed: missing role")
	}
	maxMembers := 0
	if req.Count > 0 {
		maxMembers = req.Count
	}
	resp, err := allocateRuntimeMembers(ctx, client, cfg, req.TeamID, []string{role}, maxMembers)
	meta := cloneMap(claimedMeta)
	meta["allocator_mode"] = "runtime_members"
	if resp != nil {
		if len(resp.AllocatedRoles) > 0 {
			meta["allocated_roles"] = resp.AllocatedRoles
		}
		if len(resp.MissingRoles) > 0 {
			meta["missing_roles"] = resp.MissingRoles
		}
		if len(resp.Warnings) > 0 {
			meta["allocator_warnings"] = resp.Warnings
		}
	}
	status := "allocated"
	errMsg := ""
	assigned := []map[string]any{}
	if err != nil {
		status = "error"
		errMsg = err.Error()
	} else if resp != nil {
		for _, member := range resp.RuntimeMembers {
			entry := map[string]any{
				"agent_id": member.AgentID,
				"role":     member.Role,
			}
			if member.DeploymentID != "" {
				entry["deployment_id"] = member.DeploymentID
			}
			assigned = append(assigned, entry)
		}
		if len(assigned) == 0 {
			status = "error"
			if len(resp.Warnings) > 0 {
				errMsg = strings.Join(resp.Warnings, "; ")
			} else {
				errMsg = "allocator returned no members"
			}
		}
	}
	patch := map[string]any{
		"status": status,
		"meta":   meta,
	}
	if len(assigned) > 0 {
		patch["assigned_members"] = assigned
	}
	if errMsg != "" {
		patch["error"] = errMsg
	}
	if _, err := updateSpawnRequest(ctx, client, cfg, req.TeamID, req.SpawnRequestID, patch); err != nil {
		return err
	}
	if err != nil {
		return err
	}
	return nil
}

func runCommand(ctx context.Context, cfg config, req spawnRequest, meta map[string]any) (spawnCommandOutput, error) {
	var out spawnCommandOutput
	reqJSON, _ := json.Marshal(req.Requirements)
	metaJSON, _ := json.Marshal(meta)
	cmdCtx, cancel := context.WithTimeout(ctx, cfg.commandTimeout)
	defer cancel()
	cmd := exec.CommandContext(cmdCtx, "/bin/sh", "-c", cfg.command)
	cmd.Env = append(os.Environ(),
		"SPAWN_REQUEST_ID="+req.SpawnRequestID,
		"TEAM_ID="+req.TeamID,
		"ORCHESTRATOR_RUN_ID="+req.OrchestratorRunID,
		"ROLE="+req.Role,
		"COUNT="+fmt.Sprintf("%d", req.Count),
		"STATUS="+req.Status,
		"REQUIREMENTS_JSON="+string(reqJSON),
		"META_JSON="+string(metaJSON),
	)
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	stdout, err := cmd.Output()
	if err != nil {
		msg := strings.TrimSpace(stderr.String())
		if msg == "" {
			msg = err.Error()
		}
		return out, fmt.Errorf("spawn command failed: %s", msg)
	}
	raw := strings.TrimSpace(string(stdout))
	if raw == "" {
		return out, nil
	}
	var payload map[string]json.RawMessage
	if err := json.Unmarshal([]byte(raw), &payload); err != nil {
		return out, fmt.Errorf("spawn command output must be json: %w", err)
	}
	if v, ok := payload["status"]; ok {
		_ = json.Unmarshal(v, &out.Status)
	}
	if v, ok := payload["assigned_members"]; ok {
		_ = json.Unmarshal(v, &out.AssignedMembers)
	}
	if v, ok := payload["error"]; ok {
		_ = json.Unmarshal(v, &out.Error)
	}
	if v, ok := payload["meta"]; ok {
		_ = json.Unmarshal(v, &out.Meta)
	}
	return out, nil
}

func fetchTeams(ctx context.Context, client *http.Client, cfg config) ([]teamRow, error) {
	var resp teamListResponse
	if err := doJSON(ctx, client, cfg, http.MethodGet, cfg.brokerBase+"/v1/teams", nil, &resp); err != nil {
		return nil, err
	}
	if !resp.OK {
		return nil, fmt.Errorf("broker returned ok=false for teams")
	}
	return resp.Teams, nil
}

func fetchSpawnRequests(ctx context.Context, client *http.Client, cfg config, teamID string) ([]spawnRequest, error) {
	url := fmt.Sprintf("%s/v1/teams/%s/orchestrator/spawn_requests?status=%s&limit=%d",
		cfg.brokerBase,
		teamID,
		url.QueryEscape(cfg.status),
		cfg.limit,
	)
	var resp spawnListResponse
	if err := doJSON(ctx, client, cfg, http.MethodGet, url, nil, &resp); err != nil {
		return nil, err
	}
	if !resp.OK {
		return nil, fmt.Errorf("broker returned ok=false for spawn requests")
	}
	return resp.SpawnRequests, nil
}

func updateSpawnRequest(ctx context.Context, client *http.Client, cfg config, teamID, spawnID string, payload map[string]any) (*spawnRequest, error) {
	var resp spawnResponse
	url := fmt.Sprintf("%s/v1/teams/%s/orchestrator/spawn_requests/%s", cfg.brokerBase, teamID, spawnID)
	if err := doJSON(ctx, client, cfg, http.MethodPatch, url, payload, &resp); err != nil {
		return nil, err
	}
	if !resp.OK {
		return nil, fmt.Errorf("broker returned ok=false for spawn update")
	}
	return &resp.SpawnRequest, nil
}

func allocateRuntimeMembers(ctx context.Context, client *http.Client, cfg config, teamID string, roles []string, maxMembers int) (*runtimeAllocateResponse, error) {
	payload := map[string]any{
		"roles":            roles,
		"prefer_connected": true,
	}
	if maxMembers > 0 {
		payload["max_members"] = maxMembers
	}
	var resp runtimeAllocateResponse
	url := fmt.Sprintf("%s/v1/teams/%s/runtime_members/allocate", cfg.brokerBase, teamID)
	if err := doJSON(ctx, client, cfg, http.MethodPost, url, payload, &resp); err != nil {
		return nil, err
	}
	if !resp.OK {
		return &resp, fmt.Errorf("broker returned ok=false for runtime member allocation")
	}
	return &resp, nil
}

func doJSON(ctx context.Context, client *http.Client, cfg config, method, url string, body any, out any) error {
	var reader io.Reader
	if body != nil {
		buf, err := json.Marshal(body)
		if err != nil {
			return err
		}
		reader = bytes.NewReader(buf)
	}
	req, err := http.NewRequestWithContext(ctx, method, url, reader)
	if err != nil {
		return err
	}
	token, err := cfg.bearerToken()
	if err != nil {
		return err
	}
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Accept", "application/json")
	resp, err := client.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(resp.Body)
	if err != nil {
		return err
	}
	if resp.StatusCode >= 300 {
		return &httpError{Status: resp.StatusCode, Body: strings.TrimSpace(string(data))}
	}
	if out == nil {
		return nil
	}
	if err := json.Unmarshal(data, out); err != nil {
		return err
	}
	return nil
}

func isHTTPStatus(err error, code int) bool {
	var herr *httpError
	if errors.As(err, &herr) {
		return herr.Status == code
	}
	return false
}

func httpClient(insecure bool) *http.Client {
	transport := http.DefaultTransport.(*http.Transport).Clone()
	if insecure {
		transport.TLSClientConfig = &tls.Config{InsecureSkipVerify: true}
	}
	return &http.Client{
		Timeout:   30 * time.Second,
		Transport: transport,
	}
}

func randID(n int) string {
	if n <= 0 {
		n = 8
	}
	buf := make([]byte, n)
	if _, err := rand.Read(buf); err != nil {
		return fmt.Sprintf("%d", time.Now().UnixNano())
	}
	return hex.EncodeToString(buf)[:n]
}

func cloneMap(in map[string]any) map[string]any {
	if in == nil {
		return map[string]any{}
	}
	out := make(map[string]any, len(in))
	for k, v := range in {
		out[k] = v
	}
	return out
}

func envBool(key string) bool {
	val := strings.TrimSpace(os.Getenv(key))
	if val == "" {
		return false
	}
	switch strings.ToLower(val) {
	case "1", "true", "yes", "y":
		return true
	default:
		return false
	}
}
