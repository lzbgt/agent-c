package main

import (
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
