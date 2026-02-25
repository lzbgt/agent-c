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
