package main

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"reflect"
	"strings"
	"sync"
	"testing"
	"time"
)

func TestLeaseStatusForRunUsesExplicitStatus(t *testing.T) {
	run := orchestratorRun{LeaseStatus: "Stale"}
	if got := leaseStatusForRun(run, nil); got != "stale" {
		t.Fatalf("expected stale, got %q", got)
	}
}

func TestLeaseStatusForRunMissing(t *testing.T) {
	meta := map[string]any{"lease_timeout_ms": int64(1000)}
	if got := leaseStatusForRun(orchestratorRun{}, meta); got != "missing" {
		t.Fatalf("expected missing, got %q", got)
	}
}

func TestLeaseStatusForRunOK(t *testing.T) {
	now := time.Now().UTC().UnixMilli()
	hb := now - 100
	timeout := int64(10_000)
	run := orchestratorRun{LastHeartbeatUnix: &hb, LeaseTimeoutMS: &timeout}
	if got := leaseStatusForRun(run, nil); got != "ok" {
		t.Fatalf("expected ok, got %q", got)
	}
}

func TestLeaseStatusForRunStale(t *testing.T) {
	now := time.Now().UTC().UnixMilli()
	hb := now - 2000
	timeout := int64(500)
	run := orchestratorRun{LastHeartbeatUnix: &hb, LeaseTimeoutMS: &timeout}
	if got := leaseStatusForRun(run, nil); got != "stale" {
		t.Fatalf("expected stale, got %q", got)
	}
}

func TestLeaseStatusForRunUnknown(t *testing.T) {
	now := time.Now().UTC().UnixMilli()
	hb := now - 100
	run := orchestratorRun{LastHeartbeatUnix: &hb}
	if got := leaseStatusForRun(run, nil); got != "unknown" {
		t.Fatalf("expected unknown, got %q", got)
	}
}

func TestAllowLeaseTakeover(t *testing.T) {
	if !allowLeaseTakeover(nil) {
		t.Fatalf("expected nil meta to allow takeover")
	}
	if !allowLeaseTakeover(map[string]any{}) {
		t.Fatalf("expected empty meta to allow takeover")
	}
	if allowLeaseTakeover(map[string]any{"allow_takeover": false}) {
		t.Fatalf("expected allow_takeover=false to block takeover")
	}
}

func TestRuntimeMemberRetireStatus(t *testing.T) {
	if got := runtimeMemberRetireStatus(nil); got != "paused" {
		t.Fatalf("expected paused, got %q", got)
	}
	meta := map[string]any{"retire_runtime_member_status": "ACTIVE"}
	if got := runtimeMemberRetireStatus(meta); got != "active" {
		t.Fatalf("expected active, got %q", got)
	}
}

func TestParseRuntimeMembers(t *testing.T) {
	input := []map[string]any{{"member_id": "rt-1"}, {"member_id": "rt-2"}}
	out := parseRuntimeMembers(input)
	if !reflect.DeepEqual(out, input) {
		t.Fatalf("expected parsed runtime members to match input")
	}
	mixed := []any{map[string]any{"member_id": "rt-3"}, "bad", 42}
	out = parseRuntimeMembers(mixed)
	if len(out) != 1 {
		t.Fatalf("expected 1 valid runtime member, got %d", len(out))
	}
	if out[0]["member_id"] != "rt-3" {
		t.Fatalf("unexpected runtime member: %#v", out[0])
	}
}

func TestBuildRuntimeMemberUpdates(t *testing.T) {
	runtimeMembers := []map[string]any{
		{"member_id": "rt-1", "agent_id": "agent-1", "role": "planner"},
		{"member_id": "rt-2", "agent_id": "agent-2", "role": "executor", "deployment_id": "dep-2"},
		{"member_id": "", "agent_id": "agent-3", "role": "bad"},
		{"member_id": "rt-4", "agent_id": "", "role": "bad"},
	}
	updates := buildRuntimeMemberUpdates(runtimeMembers, "paused")
	if len(updates) != 2 {
		t.Fatalf("expected 2 updates, got %d", len(updates))
	}
	if updates[0]["status"] != "paused" || updates[1]["status"] != "paused" {
		t.Fatalf("expected paused status in updates: %#v", updates)
	}
	if updates[1]["deployment_id"] != "dep-2" {
		t.Fatalf("expected deployment_id preserved: %#v", updates[1])
	}
}

func TestParseSpawnMetaConfig(t *testing.T) {
	meta := map[string]any{
		"spawn_count_per_role": 3,
		"spawn_count_by_role": map[string]any{
			"planner": 2,
		},
		"spawn_requirements": map[string]any{
			"region": "us",
		},
		"spawn_requirements_by_role": map[string]any{
			"planner": map[string]any{"tier": "gpu"},
		},
	}
	cfg := parseSpawnMetaConfig(meta)
	if cfg.defaultCount != 3 {
		t.Fatalf("expected defaultCount=3, got %d", cfg.defaultCount)
	}
	if cfg.countByRole["planner"] != 2 {
		t.Fatalf("expected planner count 2, got %d", cfg.countByRole["planner"])
	}
	if cfg.requirements["region"] != "us" {
		t.Fatalf("expected requirements region=us, got %#v", cfg.requirements)
	}
	if cfg.requirementsByRole["planner"]["tier"] != "gpu" {
		t.Fatalf("expected planner tier gpu, got %#v", cfg.requirementsByRole["planner"])
	}
	if len(cfg.errors) != 0 {
		t.Fatalf("expected no errors, got %#v", cfg.errors)
	}
}

func TestParseSpawnMetaConfigErrors(t *testing.T) {
	meta := map[string]any{
		"spawn_count_per_role": 0,
		"spawn_count_by_role": map[string]any{
			"planner": 0,
		},
		"spawn_requirements": "bad",
		"spawn_requirements_by_role": map[string]any{
			"executor": "bad",
		},
	}
	cfg := parseSpawnMetaConfig(meta)
	if len(cfg.errors) == 0 {
		t.Fatalf("expected errors for invalid spawn meta")
	}
}

func TestNormalizeRuntimeMembers(t *testing.T) {
	input := []map[string]any{
		{"agent_id": "agent-1", "role": "planner", "deployment_id": "dep-1", "status": "active"},
		{"agent_id": "agent-2", "role": "executor"},
		{"agent_id": "", "role": "bad"},
		{"agent_id": "agent-3", "role": ""},
		nil,
	}
	out := normalizeRuntimeMembers(input)
	if len(out) != 2 {
		t.Fatalf("expected 2 runtime members, got %d", len(out))
	}
	if out[0]["agent_id"] != "agent-1" || out[0]["role"] != "planner" {
		t.Fatalf("unexpected entry: %#v", out[0])
	}
	if out[0]["deployment_id"] != "dep-1" || out[0]["status"] != "active" {
		t.Fatalf("expected deployment/status preserved: %#v", out[0])
	}
}

func TestNormalizeRolesAndSignature(t *testing.T) {
	roles := []string{"Planner", "executor", "planner", "  reviewer "}
	normalized := normalizeRoles(roles)
	expected := []string{"executor", "planner", "reviewer"}
	if !reflect.DeepEqual(normalized, expected) {
		t.Fatalf("expected %v, got %v", expected, normalized)
	}
	if sig := rolesSignature(roles); sig != "executor|planner|reviewer" {
		t.Fatalf("unexpected signature: %q", sig)
	}
}

func TestResolveAllocatorMissing(t *testing.T) {
	meta := map[string]any{
		"allocator_last_team_run_id":       "run-1",
		"allocator_last_missing_signature": rolesSignature([]string{"planner"}),
		"allocator_missing_roles":          []string{"planner"},
	}
	missing := resolveAllocatorMissing(meta, "run-1", []string{"planner"})
	if !reflect.DeepEqual(missing, []string{"planner"}) {
		t.Fatalf("expected allocator missing roles, got %v", missing)
	}
	fallback := resolveAllocatorMissing(meta, "run-1", []string{"planner", "executor"})
	if !reflect.DeepEqual(fallback, normalizeRoles([]string{"planner", "executor"})) {
		t.Fatalf("expected fallback missing roles, got %v", fallback)
	}
}

func TestShouldAttemptAllocator(t *testing.T) {
	now := time.Now().UTC().UnixMilli()
	meta := map[string]any{
		"allocator_last_team_run_id":       "run-1",
		"allocator_last_missing_signature": "planner",
		"allocator_last_unix_ms":           now,
	}
	if shouldAttemptAllocator(meta, "run-1", "planner") {
		t.Fatalf("expected attempt false without retry window")
	}
	meta["allocator_retry_after_ms"] = int64(10)
	meta["allocator_last_unix_ms"] = now - 100
	if !shouldAttemptAllocator(meta, "run-1", "planner") {
		t.Fatalf("expected attempt true after retry window")
	}
	if !shouldAttemptAllocator(meta, "run-1", "executor") {
		t.Fatalf("expected attempt true for different signature")
	}
	if !shouldAttemptAllocator(meta, "run-2", "planner") {
		t.Fatalf("expected attempt true for different run")
	}
}

func TestShouldAutoAllocateRuntimeMembers(t *testing.T) {
	if !shouldAutoAllocateRuntimeMembers(nil, nil) {
		t.Fatalf("expected default auto-allocate true")
	}
	meta := map[string]any{"auto_allocate_roles": false}
	if shouldAutoAllocateRuntimeMembers(meta, &teamRunResponse{}) {
		t.Fatalf("expected meta override to disable auto-allocate")
	}
	status := &teamRunResponse{AutoAllocateRoles: false}
	if shouldAutoAllocateRuntimeMembers(nil, status) {
		t.Fatalf("expected status auto_allocate_roles=false to disable auto-allocate")
	}
}

func TestMaybeAllocateRuntimeMembersUpdatesRuntimeMembers(t *testing.T) {
	var allocatePayload map[string]any
	var updatePayload map[string]any
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") == "" {
			t.Fatalf("missing Authorization header")
		}
		switch r.URL.Path {
		case "/v1/teams/team1/runtime_members/allocate":
			if r.Method != http.MethodPost {
				t.Fatalf("allocate expected POST, got %s", r.Method)
			}
			body, _ := io.ReadAll(r.Body)
			_ = json.Unmarshal(body, &allocatePayload)
			w.Header().Set("Content-Type", "application/json")
			io.WriteString(w, `{"ok":true,"team_id":"team1","runtime_members":[{"agent_id":"agent-1","role":"planner"}],"allocated_roles":["planner"],"missing_roles":[]}`)
		case "/v1/teams/team1/runs/run1/runtime_members":
			if r.Method != http.MethodPatch {
				t.Fatalf("runtime_members expected PATCH, got %s", r.Method)
			}
			body, _ := io.ReadAll(r.Body)
			_ = json.Unmarshal(body, &updatePayload)
			w.Header().Set("Content-Type", "application/json")
			io.WriteString(w, `{"ok":true}`)
		default:
			t.Fatalf("unexpected path: %s", r.URL.Path)
		}
	}))
	defer server.Close()

	cfg := config{brokerBase: server.URL, oidcToken: "token"}
	status := &teamRunResponse{
		RuntimeMembers: []map[string]any{{"agent_id": "agent-2", "role": "executor"}},
		AutoAllocateMaxMembers: 3,
	}
	meta := map[string]any{"auto_allocate_max_members": 3}
	ok, missing, err := maybeAllocateRuntimeMembers(context.Background(), server.Client(), cfg, "team1", "run1", status, meta, []string{"planner"})
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if !ok {
		t.Fatalf("expected allocator attempt to change meta")
	}
	if len(missing) != 0 {
		t.Fatalf("expected no missing roles, got %v", missing)
	}
	if roles, ok := allocatePayload["roles"].([]any); !ok || len(roles) != 1 {
		t.Fatalf("unexpected allocate roles payload: %#v", allocatePayload["roles"])
	} else if role, ok := roles[0].(string); !ok || strings.TrimSpace(role) != "planner" {
		t.Fatalf("unexpected allocate role: %#v", roles[0])
	}
	if allocatePayload["exclude_team_members"] != true {
		t.Fatalf("expected exclude_team_members true, got %#v", allocatePayload["exclude_team_members"])
	}
	if allocatePayload["max_members"] != float64(3) {
		t.Fatalf("expected max_members 3, got %#v", allocatePayload["max_members"])
	}
	if updatePayload["mode"] != "merge" {
		t.Fatalf("expected merge mode, got %#v", updatePayload["mode"])
	}
	members, ok := updatePayload["runtime_members"].([]any)
	if !ok || len(members) != 1 {
		t.Fatalf("expected runtime_members update, got %#v", updatePayload["runtime_members"])
	}
	member, _ := members[0].(map[string]any)
	if member == nil {
		t.Fatalf("unexpected runtime member payload: %#v", members[0])
	}
	agentID, _ := member["agent_id"].(string)
	if strings.TrimSpace(agentID) != "agent-1" {
		t.Fatalf("unexpected runtime member payload: %#v", member)
	}
}

func TestMaybeAllocateRuntimeMembersNonFatal(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/v1/teams/team1/runtime_members/allocate" {
			t.Fatalf("unexpected path: %s", r.URL.Path)
		}
		http.Error(w, "no connected agents available", http.StatusBadRequest)
	}))
	defer server.Close()

	cfg := config{brokerBase: server.URL, oidcToken: "token"}
	meta := map[string]any{}
	ok, missing, err := maybeAllocateRuntimeMembers(context.Background(), server.Client(), cfg, "team1", "run1", &teamRunResponse{}, meta, []string{"planner"})
	if err != nil {
		t.Fatalf("expected non-fatal error, got %v", err)
	}
	if !ok {
		t.Fatalf("expected meta to be updated on non-fatal error")
	}
	if len(missing) != 1 || missing[0] != "planner" {
		t.Fatalf("unexpected missing roles: %v", missing)
	}
}

func TestHandleRunAllocatorFallbacksToSpawn(t *testing.T) {
	type state struct {
		mu            sync.Mutex
		calls         []string
		allocateBody  map[string]any
		spawnBody     map[string]any
		updateBodies  []map[string]any
		err           string
	}
	st := &state{}
	runMeta := map[string]any{
		"orchestrator_owner": "orch1",
		"active_team_run_id": "teamrun1",
		"auto_allocate_roles": true,
	}
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") == "" {
			st.mu.Lock()
			st.err = "missing Authorization header"
			st.mu.Unlock()
			http.Error(w, "unauthorized", http.StatusUnauthorized)
			return
		}
		switch {
		case r.Method == http.MethodPost && r.URL.Path == "/v1/teams/team1/orchestrator/runs/run1/heartbeat":
			w.Header().Set("Content-Type", "application/json")
			io.WriteString(w, `{"ok":true,"team_id":"team1","run":{"orchestrator_run_id":"run1","team_id":"team1","status":"running","meta":{"orchestrator_owner":"orch1","active_team_run_id":"teamrun1","auto_allocate_roles":true}}}`)
		case r.Method == http.MethodPatch && r.URL.Path == "/v1/teams/team1/orchestrator/runs/run1":
			body, _ := io.ReadAll(r.Body)
			var payload map[string]any
			_ = json.Unmarshal(body, &payload)
			st.mu.Lock()
			st.updateBodies = append(st.updateBodies, payload)
			st.mu.Unlock()
			w.Header().Set("Content-Type", "application/json")
			io.WriteString(w, `{"ok":true,"team_id":"team1","run":{"orchestrator_run_id":"run1","team_id":"team1","status":"running","meta":{}}}`)
		case r.Method == http.MethodGet && r.URL.Path == "/v1/teams/team1/runs/teamrun1":
			w.Header().Set("Content-Type", "application/json")
			io.WriteString(w, `{"ok":true,"team_id":"team1","team_run_id":"teamrun1","status":"running","auto_allocate_roles":true,"auto_allocate_max_members":2,"auto_allocate_missing_roles":["planner"],"runtime_members":[{"agent_id":"agent-2","role":"executor"}]}`)
		case r.Method == http.MethodGet && r.URL.Path == "/v1/teams/team1/guidance":
			w.Header().Set("Content-Type", "application/json")
			io.WriteString(w, `{"ok":true,"team_id":"team1","guidance":[],"count":0}`)
		case r.Method == http.MethodPost && r.URL.Path == "/v1/teams/team1/runtime_members/allocate":
			body, _ := io.ReadAll(r.Body)
			var payload map[string]any
			_ = json.Unmarshal(body, &payload)
			st.mu.Lock()
			st.calls = append(st.calls, "allocate")
			st.allocateBody = payload
			st.mu.Unlock()
			w.Header().Set("Content-Type", "application/json")
			io.WriteString(w, `{"ok":true,"team_id":"team1","runtime_members":[],"allocated_roles":[],"missing_roles":["planner"]}`)
		case r.Method == http.MethodPatch && r.URL.Path == "/v1/teams/team1/runs/teamrun1/runtime_members":
			st.mu.Lock()
			st.err = "unexpected runtime member update"
			st.mu.Unlock()
			http.Error(w, "unexpected", http.StatusBadRequest)
		case r.Method == http.MethodGet && strings.HasPrefix(r.URL.Path, "/v1/teams/team1/orchestrator/spawn_requests"):
			w.Header().Set("Content-Type", "application/json")
			io.WriteString(w, `{"ok":true,"team_id":"team1","spawn_requests":[]}`)
		case r.Method == http.MethodPost && r.URL.Path == "/v1/teams/team1/orchestrator/spawn_requests":
			body, _ := io.ReadAll(r.Body)
			var payload map[string]any
			_ = json.Unmarshal(body, &payload)
			st.mu.Lock()
			st.calls = append(st.calls, "spawn")
			st.spawnBody = payload
			st.mu.Unlock()
			w.Header().Set("Content-Type", "application/json")
			io.WriteString(w, `{"ok":true,"team_id":"team1","spawn_request":{"spawn_request_id":"spawn1","role":"planner","status":"requested","count":1}}`)
		default:
			st.mu.Lock()
			st.err = "unexpected request: " + r.Method + " " + r.URL.Path
			st.mu.Unlock()
			http.Error(w, "unexpected", http.StatusNotFound)
		}
	}))
	defer server.Close()

	cfg := config{brokerBase: server.URL, oidcToken: "token", orchestratorID: "orch1"}
	run := orchestratorRun{
		OrchestratorRunID: "run1",
		TeamID:            "team1",
		Status:            "running",
		Meta:              runMeta,
	}
	if err := handleRun(context.Background(), server.Client(), cfg, "team1", run); err != nil {
		t.Fatalf("handleRun error: %v", err)
	}

	st.mu.Lock()
	defer st.mu.Unlock()
	if st.err != "" {
		t.Fatalf("server error: %s", st.err)
	}
	if len(st.calls) < 2 || st.calls[0] != "allocate" || st.calls[1] != "spawn" {
		t.Fatalf("expected allocate then spawn, got %v", st.calls)
	}
	if st.allocateBody["exclude_team_members"] != true {
		t.Fatalf("expected exclude_team_members true, got %#v", st.allocateBody["exclude_team_members"])
	}
	role, _ := st.spawnBody["role"].(string)
	if strings.TrimSpace(role) != "planner" {
		t.Fatalf("unexpected spawn role payload: %#v", st.spawnBody)
	}
	if len(st.updateBodies) == 0 {
		t.Fatalf("expected orchestrator meta updates")
	}
}

func TestProcessGuidanceDispatchAndAck(t *testing.T) {
	type state struct {
		mu              sync.Mutex
		acked           []string
		directiveMeta   map[string]any
		directiveTarget map[string]any
		err             string
	}
	st := &state{}
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") == "" {
			http.Error(w, "unauthorized", http.StatusUnauthorized)
			return
		}
		switch {
		case r.Method == http.MethodGet && r.URL.Path == "/v1/teams/team1/guidance":
			q := r.URL.Query()
			if q.Get("team_run_id") == "run1" {
				w.Header().Set("Content-Type", "application/json")
				io.WriteString(w, `{"ok":true,"team_id":"team1","team_run_id":"run1","count":1,"guidance":[{"guidance_id":"g1","team_id":"team1","team_run_id":"run1","kind":"directive","priority":"normal","message":"hi","target_roles":["planner"],"status":"open","created_unix_ms":123}]}`)
				return
			}
			w.Header().Set("Content-Type", "application/json")
			io.WriteString(w, `{"ok":true,"team_id":"team1","count":0,"guidance":[]}`)
		case r.Method == http.MethodPost && r.URL.Path == "/v1/teams/team1/runs/run1/moderator/directive":
			body, _ := io.ReadAll(r.Body)
			var payload map[string]any
			_ = json.Unmarshal(body, &payload)
			meta, _ := payload["metadata"].(map[string]any)
			targets, _ := payload["targets"].(map[string]any)
			st.mu.Lock()
			st.directiveMeta = meta
			st.directiveTarget = targets
			st.mu.Unlock()
			w.Header().Set("Content-Type", "application/json")
			io.WriteString(w, `{"ok":true,"team_id":"team1","team_run_id":"run1","dispatched":[{}],"skipped":[]}`)
		case r.Method == http.MethodPost && r.URL.Path == "/v1/teams/team1/guidance/g1/ack":
			st.mu.Lock()
			st.acked = append(st.acked, "g1")
			st.mu.Unlock()
			w.Header().Set("Content-Type", "application/json")
			io.WriteString(w, `{"ok":true,"team_id":"team1","guidance":{"guidance_id":"g1","status":"acked"}}`)
		default:
			st.mu.Lock()
			st.err = "unexpected request: " + r.Method + " " + r.URL.Path
			st.mu.Unlock()
			http.Error(w, "unexpected", http.StatusNotFound)
		}
	}))
	defer server.Close()

	cfg := config{brokerBase: server.URL, oidcToken: "token", orchestratorID: "orch1"}
	meta := map[string]any{}
	changed, err := processGuidance(context.Background(), server.Client(), cfg, "team1", "run1", meta)
	if err != nil {
		t.Fatalf("processGuidance error: %v", err)
	}
	if !changed {
		t.Fatalf("expected meta change after guidance handling")
	}
	if meta["guidance_since_ts"] != int64(123) {
		t.Fatalf("expected guidance_since_ts=123, got %#v", meta["guidance_since_ts"])
	}
	st.mu.Lock()
	defer st.mu.Unlock()
	if st.err != "" {
		t.Fatalf("server error: %s", st.err)
	}
	if len(st.acked) != 1 {
		t.Fatalf("expected guidance ack, got %v", st.acked)
	}
	if st.directiveMeta == nil || st.directiveMeta["guidance_id"] != "g1" {
		t.Fatalf("expected directive metadata guidance_id, got %#v", st.directiveMeta)
	}
	if roles, ok := st.directiveTarget["roles"].([]any); !ok || len(roles) != 1 {
		t.Fatalf("expected directive role target, got %#v", st.directiveTarget)
	}
}

func TestProcessGuidanceSkipsOtherOrchestrator(t *testing.T) {
	var acked bool
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.Method == http.MethodGet && r.URL.Path == "/v1/teams/team1/guidance":
			w.Header().Set("Content-Type", "application/json")
			io.WriteString(w, `{"ok":true,"team_id":"team1","team_run_id":"run1","count":1,"guidance":[{"guidance_id":"g2","team_id":"team1","team_run_id":"run1","kind":"directive","priority":"normal","message":"hi","target_orchestrator_id":"other","status":"open","created_unix_ms":9}]}`)
		case r.Method == http.MethodPost && r.URL.Path == "/v1/teams/team1/guidance/g2/ack":
			acked = true
			http.Error(w, "unexpected ack", http.StatusBadRequest)
		default:
			w.Header().Set("Content-Type", "application/json")
			io.WriteString(w, `{"ok":true}`)
		}
	}))
	defer server.Close()

	cfg := config{brokerBase: server.URL, oidcToken: "token", orchestratorID: "orch1"}
	meta := map[string]any{}
	changed, err := processGuidance(context.Background(), server.Client(), cfg, "team1", "run1", meta)
	if err != nil {
		t.Fatalf("processGuidance error: %v", err)
	}
	if !changed {
		t.Fatalf("expected guidance cursor update for other orchestrator guidance")
	}
	if meta["guidance_since_ts"] != int64(9) {
		t.Fatalf("expected guidance_since_ts=9, got %#v", meta["guidance_since_ts"])
	}
	if acked {
		t.Fatalf("did not expect ack for other orchestrator guidance")
	}
}
