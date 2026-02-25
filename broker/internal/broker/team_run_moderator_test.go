package broker

import "testing"

func TestFilterTeamRunModeratorTargets(t *testing.T) {
	members := []teamRunMemberInfo{
		{MemberID: "m1", AgentID: "a1", Role: "planner"},
		{MemberID: "m2", AgentID: "a2", Role: "executor"},
		{MemberID: "m3", AgentID: "a3", Role: "reviewer"},
	}
	selected, skipped := filterTeamRunModeratorTargets(members, &teamRunModeratorTargets{
		Roles:     []string{"planner"},
		MemberIDs: []string{"m2"},
		AgentIDs:  []string{"a3"},
	})
	if len(selected) != 3 {
		t.Fatalf("expected 3 selected, got %d", len(selected))
	}
	if len(skipped) != 0 {
		t.Fatalf("expected no skipped, got %d", len(skipped))
	}
}

func TestFilterTeamRunModeratorTargetsNone(t *testing.T) {
	members := []teamRunMemberInfo{
		{MemberID: "m1", AgentID: "a1", Role: "planner"},
	}
	selected, skipped := filterTeamRunModeratorTargets(members, nil)
	if len(selected) != 1 {
		t.Fatalf("expected 1 selected, got %d", len(selected))
	}
	if len(skipped) != 0 {
		t.Fatalf("expected no skipped, got %d", len(skipped))
	}
}

func TestParseTeamRunModeratorTargets(t *testing.T) {
	q := map[string][]string{
		"roles":      {"planner, executor"},
		"member_ids": {"m1, m2"},
		"agent_ids":  {"a1"},
	}
	targets := parseTeamRunModeratorTargets(q)
	if targets == nil {
		t.Fatalf("expected targets")
	}
	if len(targets.Roles) != 2 {
		t.Fatalf("expected 2 roles, got %d", len(targets.Roles))
	}
	if len(targets.MemberIDs) != 2 {
		t.Fatalf("expected 2 member_ids, got %d", len(targets.MemberIDs))
	}
	if len(targets.AgentIDs) != 1 {
		t.Fatalf("expected 1 agent_id, got %d", len(targets.AgentIDs))
	}
}
