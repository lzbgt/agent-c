package broker

import (
	"reflect"
	"strings"
	"testing"
	"time"

	"agentd-broker/internal/db"
	"agentd-broker/internal/events"
)

func TestFilterTeamRunMembers(t *testing.T) {
	members := []db.TeamMember{
		{MemberID: "m1", Role: "planner", Status: "active"},
		{MemberID: "m2", Role: "executor", Status: "active"},
		{MemberID: "m3", Role: "reviewer", Status: "paused"},
		{MemberID: "m4", Role: "planner", Status: "ACTIVE"},
	}

	out := filterTeamRunMembers(members, "", nil)
	if len(out) != 3 {
		t.Fatalf("expected 3 active members, got %d", len(out))
	}

	out = filterTeamRunMembers(members, "planner", nil)
	if len(out) != 2 {
		t.Fatalf("expected 2 planner members, got %d", len(out))
	}

	out = filterTeamRunMembers(members, "", map[string]bool{"executor": true})
	if len(out) != 1 || out[0].MemberID != "m2" {
		t.Fatalf("expected executor member m2, got %+v", out)
	}
}

func TestPublishTeamQuorumResultDecisionTokens(t *testing.T) {
	hub := events.New()
	ch, cancel := hub.Subscribe("user-1")
	defer cancel()

	readEvent := func(label string) events.Event {
		t.Helper()
		select {
		case ev := <-ch:
			return ev
		case <-time.After(500 * time.Millisecond):
			t.Fatalf("timeout waiting for event: %s", label)
		}
		return events.Event{}
	}

	approveEval := teamRunQuorumEval{
		Rules: []teamRunQuorumRuleEval{
			{
				RuleID:       "rule-approve",
				QuorumMode:   "strict",
				MinApprovals: 2,
				Approved:     2,
				Missing:      0,
			},
		},
	}
	publishTeamQuorumResult(hub, "user-1", "team-1", "run-1", approveEval, "trace-1")
	ev := readEvent("approve")
	if ev.Payload["decision"] != "approve" {
		t.Fatalf("expected decision approve, got %v", ev.Payload["decision"])
	}

	denyEval := teamRunQuorumEval{
		Rules: []teamRunQuorumRuleEval{
			{
				RuleID:       "rule-deny",
				QuorumMode:   "strict",
				MinApprovals: 2,
				Approved:     1,
				Missing:      1,
			},
		},
	}
	publishTeamQuorumResult(hub, "user-1", "team-1", "run-1", denyEval, "trace-2")
	ev = readEvent("deny")
	if ev.Payload["decision"] != "deny" {
		t.Fatalf("expected decision deny, got %v", ev.Payload["decision"])
	}

	bestEffortEval := teamRunQuorumEval{
		Rules: []teamRunQuorumRuleEval{
			{
				RuleID:       "rule-best-effort",
				QuorumMode:   "best_effort",
				MinApprovals: 2,
				Approved:     1,
				Missing:      1,
			},
		},
	}
	publishTeamQuorumResult(hub, "user-1", "team-1", "run-1", bestEffortEval, "trace-3")
	ev = readEvent("best_effort")
	if ev.Payload["decision"] != "best_effort" {
		t.Fatalf("expected decision best_effort, got %v", ev.Payload["decision"])
	}
}

func TestSanitizeRunOverrides(t *testing.T) {
	raw := map[string]any{
		"model":            " gpt-4.1-mini ",
		"base_url":         " https://api.openai.com/v1 ",
		"summary_model":    "",
		"tools":            "HOST",
		"timeout_ms":       50,
		"max_steps":        999,
		"stream_assistant": true,
		"api_key":          "nope",
		"extra":            "ignore",
	}
	got := sanitizeRunOverrides(raw)
	want := map[string]any{
		"model":            "gpt-4.1-mini",
		"base_url":         "https://api.openai.com/v1",
		"tools":            "host",
		"timeout_ms":       100,
		"max_steps":        256,
		"stream_assistant": true,
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("sanitizeRunOverrides mismatch: got=%v want=%v", got, want)
	}
}

func TestParseTeamRunOverridesExplicit(t *testing.T) {
	meta := map[string]any{
		"run_overrides_mode": "explicit",
		"member_overrides": map[string]any{
			"m1": map[string]any{
				"model":      "gpt-4.1-mini",
				"timeout_ms": 250,
				"api_key":    "nope",
			},
		},
	}
	got, err := parseTeamRunOverrides(meta)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if got.Mode != "explicit" {
		t.Fatalf("expected mode explicit, got %q", got.Mode)
	}
	want := map[string]map[string]any{
		"m1": {
			"model":      "gpt-4.1-mini",
			"timeout_ms": 250,
		},
	}
	if !reflect.DeepEqual(got.MemberOverrides, want) {
		t.Fatalf("member_overrides mismatch: got=%v want=%v", got.MemberOverrides, want)
	}
}

func TestParseTeamRunOverridesRejectsBadMember(t *testing.T) {
	meta := map[string]any{
		"run_overrides_mode": "explicit",
		"member_overrides": map[string]any{
			"m1": "nope",
		},
	}
	_, err := parseTeamRunOverrides(meta)
	if err == nil {
		t.Fatalf("expected error for non-object member override")
	}
}

func TestBuildRuntimeMembers(t *testing.T) {
	meta := map[string]any{
		"runtime_members": []any{
			map[string]any{
				"member_id":    "rt-1",
				"agent_id":     "agent-a",
				"role":         "executor",
				"capabilities": []any{"vision"},
				"meta": map[string]any{
					"backend_label": "openai-mini",
					"run_overrides": map[string]any{
						"tools":   "HOST",
						"api_key": "nope",
					},
				},
			},
			map[string]any{
				"agent_id": "agent-b",
				"role":     "planner",
			},
		},
	}
	inputs, err := parseTeamRunRuntimeMembers(meta)
	if err != nil {
		t.Fatalf("unexpected parse error: %v", err)
	}
	members, _, err := buildRuntimeMembers(inputs, "team-1", map[string]bool{"m1": true})
	if err != nil {
		t.Fatalf("unexpected build error: %v", err)
	}
	if len(members) != 2 {
		t.Fatalf("expected 2 runtime members, got %d", len(members))
	}
	if members[0].MemberID != "rt-1" {
		t.Fatalf("expected member_id rt-1, got %s", members[0].MemberID)
	}
	if members[1].MemberID == "" || !strings.HasPrefix(members[1].MemberID, "rtm_") {
		t.Fatalf("expected generated runtime member_id, got %s", members[1].MemberID)
	}
	metaOut := members[0].Meta()
	ro, ok := metaOut["run_overrides"].(map[string]any)
	if !ok {
		t.Fatalf("expected run_overrides meta")
	}
	if _, ok := ro["api_key"]; ok {
		t.Fatalf("unexpected api_key in run_overrides")
	}
	if ro["tools"] != "host" {
		t.Fatalf("expected tools host, got %v", ro["tools"])
	}
}

func TestBuildRuntimeMembersDuplicateID(t *testing.T) {
	inputs := []teamRuntimeMemberInput{
		{MemberID: "rt-1", AgentID: "agent-a", Role: "executor"},
		{MemberID: "rt-1", AgentID: "agent-b", Role: "planner"},
	}
	_, _, err := buildRuntimeMembers(inputs, "team-1", map[string]bool{})
	if err == nil {
		t.Fatalf("expected duplicate member_id error")
	}
}

func TestMergeRuntimeMemberInputs(t *testing.T) {
	existing := []teamRuntimeMemberInput{
		{MemberID: "rt-1", AgentID: "agent-a", Role: "executor"},
		{MemberID: "rt-2", AgentID: "agent-b", Role: "planner"},
	}
	incoming := []teamRuntimeMemberInput{
		{MemberID: "rt-2", AgentID: "agent-b", Role: "reviewer"},
		{MemberID: "", AgentID: "agent-c", Role: "executor"},
	}
	merged := mergeRuntimeMemberInputs(existing, incoming)
	if len(merged) != 3 {
		t.Fatalf("expected 3 members, got %d", len(merged))
	}
	if merged[1].MemberID != "rt-2" || merged[1].Role != "reviewer" {
		t.Fatalf("expected rt-2 replaced, got %+v", merged[1])
	}
	if merged[2].AgentID != "agent-c" {
		t.Fatalf("expected appended agent-c, got %+v", merged[2])
	}
}
