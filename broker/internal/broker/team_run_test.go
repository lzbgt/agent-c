package broker

import (
	"testing"

	"agentd-broker/internal/db"
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
