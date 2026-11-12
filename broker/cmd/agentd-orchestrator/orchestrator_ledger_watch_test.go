package main

import (
	"agentd-broker/internal/workledger"
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func watchTestSpec() ledgerWatchSpec {
	return ledgerWatchSpec{Binding: workledger.Binding{AgentID: "agent1", DeploymentID: "dep1", SessionID: "session1"}, LedgerID: "work_one", Revision: 1}
}
func watchTestHint(spec ledgerWatchSpec) map[string]any {
	return map[string]any{"ok": true, "session_id": spec.Binding.SessionID,
		"state":      map[string]any{"schema": "agentd.work.v1", "ledger_id": spec.LedgerID, "revision": spec.Revision + 1},
		"work_watch": map[string]any{"schema": "agentd.work-watch.v1", "changed": true}}
}
func TestLedgerWatchResponseContract(t *testing.T) {
	spec := watchTestSpec()
	tests := []struct {
		name            string
		status          int
		edit            func(map[string]any)
		raw             string
		changed, failed bool
	}{
		{name: "changed", status: 200, changed: true},
		{name: "unchanged", status: 204},
		{name: "deleted", status: 200, changed: true, edit: func(h map[string]any) {
			h["state"].(map[string]any)["ledger_id"] = ""
			h["state"].(map[string]any)["revision"] = 0
		}},
		{name: "replaced", status: 200, changed: true, edit: func(h map[string]any) { h["state"].(map[string]any)["ledger_id"] = "work_new" }},
		{name: "legacy", status: 200, failed: true, edit: func(h map[string]any) { delete(h, "work_watch") }},
		{name: "wrong-session", status: 200, failed: true, edit: func(h map[string]any) { h["session_id"] = "other" }},
		{name: "no-change", status: 200, failed: true, edit: func(h map[string]any) { h["state"].(map[string]any)["revision"] = spec.Revision }},
		{name: "bad-id", status: 200, failed: true, edit: func(h map[string]any) { h["state"].(map[string]any)["ledger_id"] = "../other" }},
		{name: "bad-revision", status: 200, failed: true, edit: func(h map[string]any) { h["state"].(map[string]any)["revision"] = 9007199254740992 }},
		{name: "wrong-schema", status: 200, failed: true, edit: func(h map[string]any) { h["work_watch"].(map[string]any)["schema"] = "other" }},
		{name: "untrusted", status: 200, failed: true, edit: func(h map[string]any) { h["ok"] = false }},
		{name: "malformed", status: 200, failed: true, raw: "{"},
		{name: "oversized", status: 200, failed: true, raw: strings.Repeat(" ", 2*1024*1024+1)},
		{name: "unsupported", status: 404, failed: true},
		{name: "unauthorized", status: 401, failed: true},
		{name: "at-capacity", status: 429, failed: true},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				q := r.URL.Query()
				if r.Method != "GET" || r.URL.Path != "/v1/agents/agent1/proxy/api/v1/session/work" ||
					q.Get("session_id") != "session1" || q.Get("after_ledger_id") != "work_one" || q.Get("after_revision") != "1" || q.Get("wait_ms") != "4000" ||
					r.Header.Get("Authorization") != "Bearer fixture" || r.Header.Get("X-Agentd-Deployment") != "dep1" {
					t.Error("watch retargeted or lost authentication/cursor")
				}
				w.WriteHeader(test.status)
				if test.status == 204 {
					return
				}
				if test.raw != "" {
					_, _ = w.Write([]byte(test.raw))
					return
				}
				hint := watchTestHint(spec)
				if test.edit != nil {
					test.edit(hint)
				}
				_ = json.NewEncoder(w).Encode(hint)
			}))
			defer server.Close()
			changed, err := waitLedgerChange(context.Background(), server.Client(), config{brokerBase: server.URL, oidcToken: "fixture"}, spec)
			if changed != test.changed || (err != nil) != test.failed {
				t.Fatalf("changed=%v error=%v", changed, err)
			}
		})
	}
}
func TestLedgerWatchRedirectCannotRetargetAuthority(t *testing.T) {
	target := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { t.Error("watch redirect followed") }))
	defer target.Close()
	source := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { http.Redirect(w, r, target.URL, 302) }))
	defer source.Close()
	if changed, err := waitLedgerChange(context.Background(), source.Client(), config{brokerBase: source.URL, oidcToken: "fixture"}, watchTestSpec()); changed || err == nil {
		t.Fatal("redirect accepted")
	}
}
func TestLedgerWatchEligibility(t *testing.T) {
	h := newLedgerHarness(t)
	h.snapshot.State.Blockers["blocked"] = workledger.Blocker{Status: "open", RequirementIDs: []string{"a", "b"}, Reason: "fixture access", RequiredAction: "verify access", Attempts: []string{"observed denial"}}
	requireLedgerStep(t, h)
	original := copyGoal(h.run)
	spec, ok := ledgerWatchForRun(original, "worker")
	if !ok || spec != watchTestSpec() {
		t.Fatalf("managed requirement wait not watchable: %+v %v", spec, ok)
	}
	runtimeEdit := func(run *orchestratorRun, key string, value any) {
		runtime := ledgerRuntime(run.Meta)
		runtime[key] = value
		run.Meta["work_ledger_runtime"] = runtime
	}
	cases := []struct {
		name string
		edit func(*orchestratorRun)
	}{
		{"paused", func(r *orchestratorRun) { r.Status = "paused" }},
		{"done", func(r *orchestratorRun) { r.Status = "done" }},
		{"error", func(r *orchestratorRun) { r.Status = "error" }},
		{"manual-wait", func(r *orchestratorRun) { runtimeEdit(r, "managed_wait", false) }},
		{"disabled", func(r *orchestratorRun) { r.Meta["autonomous"] = false }},
		{"other-owner", func(r *orchestratorRun) { r.Meta["orchestrator_owner"] = "other" }},
		{"active", func(r *orchestratorRun) { r.Meta["active_team_run_id"] = "tr1" }},
		{"pending", func(r *orchestratorRun) { runtimeEdit(r, "dispatch", map[string]any{"state": "admitted"}) }},
		{"submitted", func(r *orchestratorRun) { runtimeEdit(r, "dispatch", map[string]any{"state": "submitted"}) }},
		{"receipt-wait", func(r *orchestratorRun) { runtimeEdit(r, "reason", "dispatch_receipt_pending") }},
		{"uninitialized", func(r *orchestratorRun) { runtimeEdit(r, "revision", 0) }},
		{"unsafe-id", func(r *orchestratorRun) { runtimeEdit(r, "ledger_id", "../escape") }},
		{"authority-change", func(r *orchestratorRun) {
			r.Meta["work_ledger"] = map[string]any{"agent_id": "other", "deployment_id": "dep1", "session_id": "session1"}
		}},
	}
	for _, test := range cases {
		t.Run(test.name, func(t *testing.T) {
			run := copyGoal(original)
			test.edit(&run)
			if _, ok := ledgerWatchForRun(run, "worker"); ok {
				t.Fatal("noneligible goal watched")
			}
		})
	}
	runtimeEdit(&original, "reason", "no_requirement_progress")
	if _, ok := ledgerWatchForRun(original, "worker"); !ok {
		t.Fatal("no-progress wait not watchable")
	}
}
