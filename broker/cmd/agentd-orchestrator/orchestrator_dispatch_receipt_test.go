package main

import (
	"agentd-broker/internal/workledger"
	"context"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestBoundGoalLostResponseRecoversWithoutResend(t *testing.T) {
	h := newLedgerHarness(t)
	h.losePost = true
	requireLedgerStep(t, h)
	if h.posts != 1 || h.run.Meta["active_team_run_id"] != "tr1" || ledgerDispatch(ledgerRuntime(h.run.Meta))["receipt_recovered"] != true {
		t.Fatal("saved original receipt not recovered")
	}
	h.teamStatus = "succeeded"
	finishRequirement(h, "a")
	finishRequirement(h, "b")
	requireLedgerStep(t, h)
	if h.run.Status != "done" || h.posts != 1 {
		t.Fatal("recovered goal did not finish from ledger evidence")
	}
}

func TestBoundGoalRestartAndLateReceiptResumeManagedWait(t *testing.T) {
	h := newLedgerHarness(t)
	h.losePost = true
	h.receiptHTTP = 503
	requireLedgerStep(t, h)
	if h.posts != 1 || !ledgerManagedWait(h.run) || ledgerReason(h) != "dispatch_receipt_unavailable" {
		t.Fatal("missing read-only receipt wait")
	}
	h.receiptHTTP = 0
	// A fresh coordinator/client needs no process-local submission state.
	cfg := h.cfg()
	if err := runOnce(context.Background(), &http.Client{}, cfg); err != nil {
		t.Fatal(err)
	}
	if h.posts != 1 || h.run.Status != "running" || h.run.Meta["active_team_run_id"] != "tr1" {
		t.Fatal("original run not recovered after restart")
	}
}

func TestBoundGoalPendingReceiptNeverRedispatchesEvenAfterRequirementsFinish(t *testing.T) {
	h := newLedgerHarness(t)
	h.losePost = true
	h.pendingReceipt = true
	requireLedgerStep(t, h)
	finishRequirement(h, "a")
	finishRequirement(h, "b")
	for i := 0; i < 3; i++ {
		requireLedgerStep(t, h)
	}
	if h.posts != 1 || h.run.Status == "done" || ledgerReason(h) != "dispatch_receipt_pending" {
		t.Fatal("unknown outcome treated as safe completion or retry")
	}
	for id, r := range h.receipts {
		r.State = "recorded"
		r.TeamRunID = "tr1"
		r.RecordedUnixMS = 2000
		h.receipts[id] = r
	}
	requireLedgerStep(t, h)
	h.teamStatus = "succeeded"
	requireLedgerStep(t, h)
	if h.posts != 1 || h.run.Status != "done" {
		t.Fatal("late saved receipt did not unblock verified completion")
	}
}

func TestBoundGoalNoAdmissionReceiptIsNotPermissionToRetry(t *testing.T) {
	h := newLedgerHarness(t)
	h.losePost = true
	h.receiptHTTP = 503
	requireLedgerStep(t, h)
	h.receipts = map[string]workledger.DispatchReceipt{}
	h.receiptHTTP = 0
	for i := 0; i < 3; i++ {
		requireLedgerStep(t, h)
	}
	if h.posts != 1 || !ledgerManagedWait(h.run) || ledgerReason(h) != "dispatch_receipt_pending" {
		t.Fatal("absent admission replayed")
	}
}

func TestBoundGoalReceiptMismatchStopsContinuation(t *testing.T) {
	cases := map[string]func(*workledger.DispatchReceipt){
		"team":       func(r *workledger.DispatchReceipt) { r.TeamID = "other" },
		"goal":       func(r *workledger.DispatchReceipt) { r.OrchestratorRunID = "other" },
		"nonce":      func(r *workledger.DispatchReceipt) { r.DispatchID = "other" },
		"agent":      func(r *workledger.DispatchReceipt) { r.Binding.AgentID = "other" },
		"deployment": func(r *workledger.DispatchReceipt) { r.Binding.DeploymentID = "other" },
		"session":    func(r *workledger.DispatchReceipt) { r.Binding.SessionID = "other" },
		"ledger":     func(r *workledger.DispatchReceipt) { r.LedgerID = "work_other" },
		"revision":   func(r *workledger.DispatchReceipt) { r.Revision++ },
		"digest":     func(r *workledger.DispatchReceipt) { r.Digest = strings.Repeat("b", 64) },
		"control":    func(r *workledger.DispatchReceipt) { r.ControlDigest = strings.Repeat("b", 64) },
		"team_run":   func(r *workledger.DispatchReceipt) { r.TeamRunID = "other" },
		"state":      func(r *workledger.DispatchReceipt) { r.State = "succeeded" },
	}
	for name, mutate := range cases {
		t.Run(name, func(t *testing.T) {
			h := newLedgerHarness(t)
			h.receiptEdit = mutate
			requireLedgerStep(t, h)
			if h.posts != 1 || h.run.Status != "waiting" || ledgerManagedWait(h.run) || ledgerReason(h) != "dispatch_receipt_mismatch" || asString(h.run.Meta["active_team_run_id"]) != "" {
				t.Fatal("mismatched receipt adopted")
			}
			requireLedgerStep(t, h)
			if h.posts != 1 {
				t.Fatal("mismatched turn retried")
			}
		})
	}
}

func TestBoundGoalReceiptAttachmentPreservesLaterOwnerControls(t *testing.T) {
	for _, stop := range []string{"paused", "waiting", "done", "error", "owner", "autonomous"} {
		t.Run(stop, func(t *testing.T) {
			h := newLedgerHarness(t)
			h.losePost = true
			h.stopOnReceipt = stop
			requireLedgerStep(t, h)
			if h.run.Meta["active_team_run_id"] != "tr1" || h.posts != 1 {
				t.Fatal("original receipt lost")
			}
			switch stop {
			case "owner":
				if h.run.Meta["orchestrator_owner"] != "another" {
					t.Fatal("owner overwritten")
				}
			case "autonomous":
				if h.run.Meta["autonomous"] != false {
					t.Fatal("autonomy reenabled")
				}
			default:
				if h.run.Status != stop {
					t.Fatal("owner stop overwritten")
				}
			}
			if stop == "waiting" && ledgerManagedWait(h.run) {
				t.Fatal("manual wait changed to automatic")
			}
			requireLedgerStep(t, h)
			if h.posts != 1 {
				t.Fatal("stopped work continued")
			}
		})
	}
}

func TestBoundGoalReceiptTransportRejectsRedirectAndOversize(t *testing.T) {
	target := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { t.Error("redirect target reached") }))
	defer target.Close()
	redirect := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { http.Redirect(w, r, target.URL, 302) }))
	defer redirect.Close()
	if _, err := fetchLedgerDispatch(context.Background(), redirect.Client(), config{brokerBase: redirect.URL, oidcToken: "fixture"}, "team1", "goal1", "nonce"); err == nil {
		t.Fatal("redirect accepted")
	}
	large := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { _, _ = w.Write([]byte(strings.Repeat(" ", 65537))) }))
	defer large.Close()
	if _, err := fetchLedgerDispatch(context.Background(), large.Client(), config{brokerBase: large.URL, oidcToken: "fixture"}, "team1", "goal1", "nonce"); err != errLedgerReceiptMismatch {
		t.Fatal("unbounded receipt accepted")
	}
}
