package main

import (
	"reflect"
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
