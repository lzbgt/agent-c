package main

import (
	"agentd-broker/internal/workledger"
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

type watchLoopHarness struct {
	ledger          *ledgerHarness
	hints           chan struct{}
	watches, cycles atomic.Int32
	mode            string
}

func newWatchLoopHarness(t *testing.T, mode string) *watchLoopHarness {
	t.Helper()
	h := newLedgerHarness(t)
	h.snapshot.State.Blockers["blocked"] = workledger.Blocker{Status: "open", RequirementIDs: []string{"a", "b"}, Reason: "fixture access", RequiredAction: "verify access", Attempts: []string{"observed denial"}}
	requireLedgerStep(t, h)
	f := &watchLoopHarness{ledger: h, hints: make(chan struct{}, 4), mode: mode}
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/v1/teams" {
			f.cycles.Add(1)
		}
		if !strings.HasSuffix(r.URL.Path, "/session/work") || r.URL.Query().Get("wait_ms") == "" {
			h.serve(w, r)
			return
		}
		f.watches.Add(1)
		if f.mode == "unsupported" {
			w.WriteHeader(404)
			return
		}
		select {
		case <-r.Context().Done():
			return
		case <-f.hints:
		}
		h.mu.Lock()
		hint := watchTestHint(watchTestSpec())
		state := hint["state"].(map[string]any)
		state["ledger_id"] = h.snapshot.State.LedgerID
		state["revision"] = h.snapshot.State.Revision
		if f.mode == "stale" {
			state["revision"] = h.snapshot.State.Revision + 1
		}
		h.mu.Unlock()
		_ = json.NewEncoder(w).Encode(hint)
	}))
	h.server = server
	t.Cleanup(server.Close)
	return f
}
func (f *watchLoopHarness) start(t *testing.T, cfg config) {
	t.Helper()
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { defer close(done); runLoop(ctx, f.ledger.server.Client(), cfg) }()
	t.Cleanup(func() {
		cancel()
		select {
		case <-done:
		case <-time.After(3 * time.Second):
			t.Error("coordinator/watch did not cancel")
		}
	})
}
func (f *watchLoopHarness) resolve() {
	h := f.ledger
	h.mu.Lock()
	defer h.mu.Unlock()
	blocker := h.snapshot.State.Blockers["blocked"]
	blocker.Status = "resolved"
	blocker.Evidence = "verified fixture access"
	h.snapshot.State.Blockers["blocked"] = blocker
	h.snapshot.State.Revision++
}
func (f *watchLoopHarness) posts() int {
	f.ledger.mu.Lock()
	defer f.ledger.mu.Unlock()
	return f.ledger.posts
}
func TestBoundGoalLedgerWatchContinuesBeforePeriodicPoll(t *testing.T) {
	for _, kind := range []string{"blocked", "no-progress"} {
		t.Run(kind, func(t *testing.T) {
			f := newWatchLoopHarness(t, "")
			h := f.ledger
			want := 1
			if kind == "no-progress" {
				f.resolve()
				requireLedgerStep(t, h)
				h.teamStatus = "succeeded"
				requireLedgerStep(t, h)
				if ledgerReason(h) != "no_requirement_progress" {
					t.Fatal("fixture did not establish no-progress wait")
				}
				want = 2
			}
			cfg := h.cfg()
			cfg.pollInterval = 30 * time.Second
			f.start(t, cfg)
			eventuallyWatch(t, func() bool { return f.watches.Load() == 1 })
			if kind == "blocked" {
				f.resolve()
			} else {
				h.mu.Lock()
				finishRequirement(h, "a")
				h.mu.Unlock()
			}
			f.hints <- struct{}{}
			eventuallyWatch(t, func() bool {
				h.mu.Lock()
				defer h.mu.Unlock()
				return h.posts == want && h.run.Status == "running" && asString(h.run.Meta["active_team_run_id"]) != ""
			})
			h.mu.Lock()
			defer h.mu.Unlock()
			if h.run.Status != "running" || asString(h.run.Meta["active_team_run_id"]) == "" {
				t.Fatal("hint did not continue original bound goal")
			}
		})
	}
}
func TestBoundGoalLedgerWatchPreservesOwnerStops(t *testing.T) {
	for _, stop := range []string{"paused", "waiting", "done", "error", "autonomous", "owner", "owner-no-takeover"} {
		t.Run(stop, func(t *testing.T) {
			f := newWatchLoopHarness(t, "")
			h := f.ledger
			cfg := h.cfg()
			cfg.pollInterval = 30 * time.Second
			f.start(t, cfg)
			eventuallyWatch(t, func() bool { return f.watches.Load() == 1 })
			h.mu.Lock()
			switch stop {
			case "autonomous":
				h.run.Meta["autonomous"] = false
			case "owner":
				h.run.Meta["orchestrator_owner"] = "other"
				h.run.LeaseStatus = "ok" // Existing authorized stale-lease takeover is not an owner stop.
			case "owner-no-takeover":
				h.run.Meta["orchestrator_owner"] = "other"
				h.run.Meta["allow_takeover"] = false
			case "waiting":
				runtime := ledgerRuntime(h.run.Meta)
				runtime["managed_wait"] = false
				h.run.Meta["work_ledger_runtime"] = runtime
			default:
				h.run.Status = stop
			}
			h.mu.Unlock()
			f.resolve()
			f.hints <- struct{}{}
			eventuallyWatch(t, func() bool { return f.cycles.Load() >= 2 })
			time.Sleep(50 * time.Millisecond)
			if f.posts() != 0 {
				t.Fatal("wake overrode owner stop")
			}
			h.mu.Lock()
			defer h.mu.Unlock()
			if strings.HasPrefix(stop, "owner") && h.run.Meta["orchestrator_owner"] != "other" {
				t.Fatal("owner changed")
			}
			if stop == "autonomous" && h.run.Meta["autonomous"] != false {
				t.Fatal("autonomy changed")
			}
			if !strings.HasPrefix(stop, "owner") && stop != "autonomous" && h.run.Status != stop {
				t.Fatal("stop status changed")
			}
		})
	}
}
func TestBoundGoalLedgerWatchHintIsNeverAuthority(t *testing.T) {
	f := newWatchLoopHarness(t, "stale")
	h := f.ledger
	cfg := h.cfg()
	cfg.pollInterval = 30 * time.Second
	f.start(t, cfg)
	eventuallyWatch(t, func() bool { return f.watches.Load() == 1 })
	h.mu.Lock()
	reads := h.reads
	h.mu.Unlock()
	f.hints <- struct{}{}
	eventuallyWatch(t, func() bool { h.mu.Lock(); defer h.mu.Unlock(); return h.reads > reads })
	time.Sleep(80 * time.Millisecond)
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.posts != 0 || h.run.Status != "waiting" || ledgerReason(h) != "requirements_blocked" || f.watches.Load() != 1 {
		t.Fatal("unverified hint admitted work or spun")
	}
}
func TestBoundGoalLedgerWatchReplacementFailsClosed(t *testing.T) {
	f := newWatchLoopHarness(t, "")
	h := f.ledger
	cfg := h.cfg()
	cfg.pollInterval = 30 * time.Second
	f.start(t, cfg)
	eventuallyWatch(t, func() bool { return f.watches.Load() == 1 })
	h.mu.Lock()
	h.snapshot.State.LedgerID = "work_recreated"
	h.snapshot.State.Revision++
	h.mu.Unlock()
	f.hints <- struct{}{}
	eventuallyWatch(t, func() bool { h.mu.Lock(); defer h.mu.Unlock(); return ledgerReason(h) == "authority_history_changed" })
	if f.posts() != 0 {
		t.Fatal("replacement admitted work")
	}
}
func TestBoundGoalLedgerWatchUnsupportedRetainsPolling(t *testing.T) {
	f := newWatchLoopHarness(t, "unsupported")
	h := f.ledger
	cfg := h.cfg()
	cfg.pollInterval = 80 * time.Millisecond
	f.start(t, cfg)
	eventuallyWatch(t, func() bool { return f.watches.Load() == 1 })
	f.resolve()
	eventuallyWatch(t, func() bool {
		h.mu.Lock()
		defer h.mu.Unlock()
		return h.posts == 1 && asString(h.run.Meta["active_team_run_id"]) != ""
	})
	if f.watches.Load() != 1 {
		t.Fatal("unsupported daemon request storm")
	}
}
func TestBoundGoalLedgerWatchOnceAndDisabledDoNotWatch(t *testing.T) {
	for _, once := range []bool{true, false} {
		t.Run(map[bool]string{true: "once", false: "disabled"}[once], func(t *testing.T) {
			f := newWatchLoopHarness(t, "")
			h := f.ledger
			cfg := h.cfg()
			cfg.once = once
			cfg.disableLedgerWatch = !once
			cfg.pollInterval = 20 * time.Millisecond
			if once {
				runLoop(context.Background(), h.server.Client(), cfg)
			} else {
				f.start(t, cfg)
				eventuallyWatch(t, func() bool { return f.cycles.Load() >= 2 })
			}
			if f.watches.Load() != 0 || f.posts() != 0 {
				t.Fatal("watch started despite polling-only request")
			}
		})
	}
}
