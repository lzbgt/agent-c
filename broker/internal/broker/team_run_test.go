package broker

import (
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
