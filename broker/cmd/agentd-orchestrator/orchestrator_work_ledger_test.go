package main

import (
	"agentd-broker/internal/workledger"
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"reflect"
	"strings"
	"sync"
	"testing"
)

type ledgerHarness struct {
	mu                                             sync.Mutex
	run                                            orchestratorRun
	snapshot                                       workledger.Snapshot
	posts, reads                                   int
	teamStatus                                     string
	losePost, pauseBeforeAdmission, pauseAfterPost bool
	ledgerHTTP                                     int
	server                                         *httptest.Server
}

func newLedgerHarness(t *testing.T) *ledgerHarness {
	t.Helper()
	h := &ledgerHarness{teamStatus: "running"}
	h.run = orchestratorRun{OrchestratorRunID: "goal1", TeamID: "team1", Status: "running", Goal: "Deliver product", Meta: map[string]any{"work_ledger": map[string]any{"agent_id": "agent1", "deployment_id": "dep1", "session_id": "session1"}}}
	h.snapshot = workledger.Snapshot{OK: true, SessionID: "session1", State: workledger.State{Schema: "agentd.work.v1", LedgerID: "work_one", Revision: 1, Requirements: map[string]workledger.Requirement{"a": {Title: "First", Status: "pending", DependsOn: []string{}}, "b": {Title: "Second", Status: "pending", DependsOn: []string{}}}, Blockers: map[string]workledger.Blocker{}}}
	h.server = httptest.NewServer(http.HandlerFunc(h.serve))
	t.Cleanup(h.server.Close)
	return h
}
func (h *ledgerHarness) cfg() config {
	return config{brokerBase: h.server.URL, oidcToken: "fixture", orchestratorID: "worker", status: "running", limit: 50}
}
func copyGoal(run orchestratorRun) orchestratorRun {
	b, _ := json.Marshal(run)
	var out orchestratorRun
	_ = json.Unmarshal(b, &out)
	return out
}
func (h *ledgerHarness) step(t *testing.T) error {
	t.Helper()
	h.mu.Lock()
	run := copyGoal(h.run)
	h.mu.Unlock()
	return handleRun(context.Background(), h.server.Client(), h.cfg(), "team1", run)
}
func (h *ledgerHarness) serve(w http.ResponseWriter, r *http.Request) {
	h.mu.Lock()
	defer h.mu.Unlock()
	w.Header().Set("Content-Type", "application/json")
	send := func(v any) { _ = json.NewEncoder(w).Encode(v) }
	goal := func() { send(map[string]any{"ok": true, "run": h.run}) }
	var body map[string]any
	if r.Method != "GET" {
		_ = json.NewDecoder(r.Body).Decode(&body)
	}
	switch {
	case strings.HasSuffix(r.URL.Path, "/session/work"):
		if r.Header.Get("X-Agentd-Deployment") != "dep1" || r.URL.Query().Get("session_id") != "session1" || r.Header.Get("Authorization") != "Bearer fixture" {
			http.Error(w, "wrong authority", 400)
			return
		}
		h.reads++
		if h.ledgerHTTP != 0 {
			http.Error(w, "unavailable", h.ledgerHTTP)
			return
		}
		send(h.snapshot)
	case strings.Contains(r.URL.Path, "/guidance"):
		send(map[string]any{"ok": true, "guidance": []any{}})
	case strings.HasSuffix(r.URL.Path, "/heartbeat"):
		if body["expected_owner"] != h.run.Meta["orchestrator_owner"] || body["expected_status"] != h.run.Status {
			http.Error(w, "conflict", 409)
			return
		}
		goal()
	case r.URL.Path == "/v1/teams/team1/orchestrator/runs/goal1":
		if r.Method == "GET" {
			goal()
			return
		}
		next, _ := body["meta"].(map[string]any)
		if h.pauseBeforeAdmission && ledgerRuntime(next)["reason"] == "dispatching" {
			h.run.Status = "paused"
		}
		if body["expected_status"] != h.run.Status || !reflect.DeepEqual(body["expected_meta"], h.run.Meta) {
			http.Error(w, "conflict", 409)
			return
		}
		h.run.Status = body["status"].(string)
		h.run.Meta = next
		goal()
	case r.URL.Path == "/v1/teams/team1/runs" && r.Method == "POST":
		h.posts++
		run := body["run"].(map[string]any)
		team := body["team"].(map[string]any)
		if run["session_id"] != "session1" || run["no_session"] != false || team["auto_allocate_roles"] != false || !strings.Contains(run["prompt"].(string), "work_state") {
			http.Error(w, "wrong bound payload", 400)
			return
		}
		runtime := ledgerRuntime(h.run.Meta)
		dispatch := ledgerDispatch(runtime)
		if h.run.Status != "running" || dispatch["id"] != team["work_ledger_dispatch_id"] || dispatch["state"] != "admitted" {
			http.Error(w, "admission conflict", 409)
			return
		}
		dispatch["state"] = "submitted"
		runtime["dispatch"] = dispatch
		h.run.Meta["work_ledger_runtime"] = runtime
		if h.losePost {
			http.Error(w, "response lost after accepted host dispatch", 502)
			return
		}
		if h.pauseAfterPost {
			h.run.Status = "paused"
		}
		send(map[string]any{"ok": true, "team_run_id": fmt.Sprintf("tr%d", h.posts), "status": "running"})
	case strings.HasPrefix(r.URL.Path, "/v1/teams/team1/runs/tr"):
		send(map[string]any{"ok": true, "team_run_id": r.URL.Path[strings.LastIndex(r.URL.Path, "/")+1:], "status": h.teamStatus})
	case r.URL.Path == "/v1/teams":
		send(map[string]any{"ok": true, "teams": []any{map[string]any{"team_id": "team1"}}})
	case r.URL.Path == "/v1/teams/team1/orchestrator/runs":
		runs := []orchestratorRun{}
		if r.URL.Query().Get("status") == h.run.Status {
			runs = append(runs, h.run)
		}
		send(map[string]any{"ok": true, "runs": runs})
	default:
		http.Error(w, "unexpected "+r.URL.Path, 404)
	}
}
func requireLedgerStep(t *testing.T, h *ledgerHarness) {
	t.Helper()
	if err := h.step(t); err != nil {
		t.Fatal(err)
	}
}
func ledgerReason(h *ledgerHarness) string { return asString(ledgerRuntime(h.run.Meta)["reason"]) }
func finishRequirement(h *ledgerHarness, id string) {
	r := h.snapshot.State.Requirements[id]
	r.Status = "done"
	r.Evidence = "verified test receipt"
	h.snapshot.State.Requirements[id] = r
	h.snapshot.State.Revision++
}

func TestBoundGoalSuccessRequiresAllRequirements(t *testing.T) {
	h := newLedgerHarness(t)
	requireLedgerStep(t, h)
	if h.posts != 1 || h.run.Meta["active_team_run_id"] != "tr1" {
		t.Fatal("initial authority turn not recorded")
	}
	h.teamStatus = "succeeded"
	finishRequirement(h, "a")
	requireLedgerStep(t, h)
	if h.run.Status == "done" || ledgerReason(h) != "requirements_ready" {
		t.Fatalf("team success falsely completed goal: %+v", h.run)
	}
	requireLedgerStep(t, h)
	if h.posts != 2 {
		t.Fatal("unfinished independent requirement not continued")
	}
	finishRequirement(h, "b")
	requireLedgerStep(t, h)
	if h.run.Status != "done" || ledgerReason(h) != "requirements_verified" {
		t.Fatalf("evidence-backed goal not done: %+v", h.run)
	}
	requireLedgerStep(t, h)
	if h.posts != 2 {
		t.Fatal("completed goal relaunched")
	}
}
func TestBoundGoalScopedBlockerResolutionWakesWait(t *testing.T) {
	h := newLedgerHarness(t)
	h.snapshot.State.Blockers["blocked"] = workledger.Blocker{Status: "open", RequirementIDs: []string{"a"}, Reason: "access", RequiredAction: "grant", Attempts: []string{"denied"}}
	requireLedgerStep(t, h)
	if h.posts != 1 {
		t.Fatal("independent requirement did not run")
	}
	finishRequirement(h, "b")
	h.teamStatus = "succeeded"
	requireLedgerStep(t, h)
	if h.run.Status != "waiting" || ledgerReason(h) != "requirements_blocked" {
		t.Fatal("expected scoped managed wait")
	}
	if err := runOnce(context.Background(), h.server.Client(), h.cfg()); err != nil {
		t.Fatal(err)
	}
	if h.posts != 1 {
		t.Fatal("blocked work dispatched")
	}
	b := h.snapshot.State.Blockers["blocked"]
	b.Status = "resolved"
	b.Evidence = "access verified"
	h.snapshot.State.Blockers["blocked"] = b
	h.snapshot.State.Revision++
	if err := runOnce(context.Background(), h.server.Client(), h.cfg()); err != nil {
		t.Fatal(err)
	}
	if h.posts != 2 || h.run.Status != "running" {
		t.Fatal("resolved blocker did not wake work")
	}
}
func TestBoundGoalAdvisoryBlockerDoesNotStop(t *testing.T) {
	h := newLedgerHarness(t)
	h.snapshot.State.Blockers["advice"] = workledger.Blocker{Status: "open", RequirementIDs: []string{}, Reason: "Advisory", RequiredAction: "Observe", Attempts: []string{"Checked independently"}}
	requireLedgerStep(t, h)
	if h.posts != 1 {
		t.Fatal("advisory blocker incorrectly global")
	}
}
func TestBoundGoalNoProgressDoesNotSpin(t *testing.T) {
	h := newLedgerHarness(t)
	requireLedgerStep(t, h)
	h.teamStatus = "succeeded"
	requireLedgerStep(t, h)
	for i := 0; i < 3; i++ {
		requireLedgerStep(t, h)
	}
	if h.posts != 1 || ledgerReason(h) != "no_requirement_progress" {
		t.Fatal("unchanged successful turn repeated")
	}
	finishRequirement(h, "a")
	requireLedgerStep(t, h)
	if h.posts != 2 {
		t.Fatal("verified progress failed to wake")
	}
}
func TestBoundGoalLostReceiptNeverReplays(t *testing.T) {
	h := newLedgerHarness(t)
	h.losePost = true
	if h.step(t) == nil {
		t.Fatal("lost receipt not reported")
	}
	for i := 0; i < 3; i++ {
		requireLedgerStep(t, h)
	}
	if h.posts != 1 || ledgerReason(h) != "dispatch_outcome_unknown" || ledgerManagedWait(h.run) {
		t.Fatal("uncertain launch replayed")
	}
}
func TestBoundGoalOwnerStopsAndPauseRace(t *testing.T) {
	for _, status := range []string{"paused", "waiting", "error", "done"} {
		t.Run(status, func(t *testing.T) {
			h := newLedgerHarness(t)
			h.run.Status = status
			requireLedgerStep(t, h)
			if h.posts != 0 || h.reads != 0 || h.run.Status != status {
				t.Fatal("owner stop overridden")
			}
		})
	}
	h := newLedgerHarness(t)
	h.pauseBeforeAdmission = true
	requireLedgerStep(t, h)
	if h.posts != 0 || h.run.Status != "paused" {
		t.Fatal("pause before admission overwritten")
	}
}
func TestBoundGoalPauseAfterAdmissionRetainsReceipt(t *testing.T) {
	h := newLedgerHarness(t)
	h.pauseAfterPost = true
	requireLedgerStep(t, h)
	if h.run.Status != "paused" || h.run.Meta["active_team_run_id"] != "tr1" {
		t.Fatal("late receipt lost or pause overwritten")
	}
	requireLedgerStep(t, h)
	if h.posts != 1 {
		t.Fatal("paused admitted work repeated")
	}
}
func TestBoundGoalFailedTeamDoesNotRetry(t *testing.T) {
	h := newLedgerHarness(t)
	requireLedgerStep(t, h)
	h.teamStatus = "failed"
	requireLedgerStep(t, h)
	requireLedgerStep(t, h)
	if h.run.Status != "error" || h.posts != 1 {
		t.Fatal("failed turn silently retried")
	}
}
func TestBoundGoalAuthorityUnavailableAndChanged(t *testing.T) {
	h := newLedgerHarness(t)
	h.ledgerHTTP = 503
	requireLedgerStep(t, h)
	if h.posts != 0 || !ledgerManagedWait(h.run) {
		t.Fatal("unavailable authority admitted")
	}
	h.ledgerHTTP = 0
	requireLedgerStep(t, h)
	if h.posts != 1 {
		t.Fatal("authority recovery did not resume")
	}
	h.teamStatus = "succeeded"
	requireLedgerStep(t, h)
	h.snapshot.State.LedgerID = "work_recreated"
	h.snapshot.State.Revision++
	requireLedgerStep(t, h)
	if ledgerReason(h) != "authority_history_changed" || h.posts != 1 {
		t.Fatal("new ledger identity adopted")
	}
}
func TestBoundGoalConcurrentSameOwnerOnlyOneLaunch(t *testing.T) {
	h := newLedgerHarness(t)
	h.run.Meta["orchestrator_owner"] = "worker"
	initial := copyGoal(h.run)
	var wg sync.WaitGroup
	for i := 0; i < 2; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			_ = handleRun(context.Background(), h.server.Client(), h.cfg(), "team1", copyGoal(initial))
		}()
	}
	wg.Wait()
	if h.posts > 1 {
		t.Fatalf("duplicate launch: %d", h.posts)
	}
}
func TestBoundGoalRedirectCannotRetargetAuthority(t *testing.T) {
	target := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { t.Error("redirect followed") }))
	defer target.Close()
	source := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { http.Redirect(w, r, target.URL, 302) }))
	defer source.Close()
	_, err := fetchWorkLedger(context.Background(), source.Client(), config{brokerBase: source.URL, oidcToken: "fixture"}, workledger.Binding{AgentID: "a", DeploymentID: "d", SessionID: "s"})
	if err == nil {
		t.Fatal("redirect accepted")
	}
}
