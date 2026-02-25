package broker

import "testing"

func TestTeamRunMemberJobSummary(t *testing.T) {
	teamMeta := map[string]any{
		"member_jobs": []map[string]any{
			{"status": "queued"},
			{"status": "running"},
			{"status": "done", "ok": true},
			{"status": "error", "error": "boom"},
			{"status": "cancelled"},
			{"status": "interrupted", "ok": false},
			{},
			{"status": "done", "dispatch_error": "missing job_id"},
		},
		"dispatch_errors": []map[string]any{
			{"member_id": "m1", "error": "dispatch failed"},
			{"member_id": "m2", "error": "dispatch failed"},
		},
	}
	summary := teamRunMemberJobSummary(teamMeta)
	if summary == nil {
		t.Fatal("expected summary")
	}
	getInt := func(key string) int {
		v, ok := summary[key]
		if !ok {
			t.Fatalf("missing summary key %q", key)
		}
		if n, ok := v.(int); ok {
			return n
		}
		if n, ok := v.(int64); ok {
			return int(n)
		}
		if n, ok := v.(float64); ok {
			return int(n)
		}
		t.Fatalf("unexpected type for %q: %T", key, v)
		return 0
	}
	if got := getInt("total"); got != 8 {
		t.Fatalf("total=%d, want 8", got)
	}
	if got := getInt("queued"); got != 1 {
		t.Fatalf("queued=%d, want 1", got)
	}
	if got := getInt("running"); got != 1 {
		t.Fatalf("running=%d, want 1", got)
	}
	if got := getInt("done"); got != 2 {
		t.Fatalf("done=%d, want 2", got)
	}
	if got := getInt("error"); got != 1 {
		t.Fatalf("error=%d, want 1", got)
	}
	if got := getInt("cancelled"); got != 1 {
		t.Fatalf("cancelled=%d, want 1", got)
	}
	if got := getInt("interrupted"); got != 1 {
		t.Fatalf("interrupted=%d, want 1", got)
	}
	if got := getInt("unknown"); got != 1 {
		t.Fatalf("unknown=%d, want 1", got)
	}
	if got := getInt("ok"); got != 1 {
		t.Fatalf("ok=%d, want 1", got)
	}
	if got := getInt("failed"); got != 4 {
		t.Fatalf("failed=%d, want 4", got)
	}
	if got := getInt("dispatch_errors"); got != 2 {
		t.Fatalf("dispatch_errors=%d, want 2", got)
	}
}

func TestAllocateRuntimeMembersByRole(t *testing.T) {
	candidates := []runtimeAgentCandidate{
		{AgentID: "agent-a", DeploymentID: "dep-a"},
		{AgentID: "agent-b", DeploymentID: "dep-b"},
	}
	allocations, allocatedRoles, missingRoles, warning := allocateRuntimeMembersByRole(
		[]string{"planner", "executor"},
		candidates,
		nil,
		nil,
		0,
	)
	if warning != "" {
		t.Fatalf("unexpected warning: %s", warning)
	}
	if len(allocations) != 2 {
		t.Fatalf("expected 2 allocations, got %d", len(allocations))
	}
	if len(missingRoles) != 0 {
		t.Fatalf("expected no missing roles, got %v", missingRoles)
	}
	if len(allocatedRoles) != 2 {
		t.Fatalf("expected 2 allocated roles, got %v", allocatedRoles)
	}
	if allocations[0].Role != "planner" || allocations[1].Role != "executor" {
		t.Fatalf("unexpected role order: %v", []string{allocations[0].Role, allocations[1].Role})
	}

	existing := map[string]bool{"planner": true}
	allocations, _, missingRoles, warning = allocateRuntimeMembersByRole(
		[]string{"planner", "executor"},
		candidates,
		existing,
		map[string]bool{},
		0,
	)
	if warning != "" {
		t.Fatalf("unexpected warning with existing role: %s", warning)
	}
	if len(allocations) != 1 || allocations[0].Role != "executor" {
		t.Fatalf("expected executor allocation, got %+v", allocations)
	}
	if len(missingRoles) != 0 {
		t.Fatalf("expected no missing roles, got %v", missingRoles)
	}

	allocations, _, missingRoles, warning = allocateRuntimeMembersByRole(
		[]string{"planner", "executor"},
		[]runtimeAgentCandidate{{AgentID: "agent-a", DeploymentID: "dep-a"}},
		nil,
		nil,
		1,
	)
	if len(allocations) != 1 {
		t.Fatalf("expected 1 allocation, got %d", len(allocations))
	}
	if len(missingRoles) != 1 {
		t.Fatalf("expected 1 missing role, got %v", missingRoles)
	}
	if warning == "" {
		t.Fatal("expected warning for insufficient allocation")
	}
}

func TestParseGoalContract(t *testing.T) {
	contract, err := parseGoalContract("  ship it ")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if contract["goal"] != "ship it" {
		t.Fatalf("unexpected goal: %v", contract["goal"])
	}

	contract, err = parseGoalContract(map[string]any{
		"goal":             "test goal",
		"success_criteria": []any{"ok", "fast"},
		"constraints":      []string{"safe"},
	})
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if contract["goal"] != "test goal" {
		t.Fatalf("unexpected goal: %v", contract["goal"])
	}
	if _, ok := contract["success_criteria"]; !ok {
		t.Fatal("expected success_criteria")
	}
	if _, ok := contract["constraints"]; !ok {
		t.Fatal("expected constraints")
	}

	if _, err := parseGoalContract(123); err == nil {
		t.Fatal("expected error for invalid goal_contract")
	}
}

func TestAppendGoalEventCapped(t *testing.T) {
	meta := map[string]any{}
	for i := 0; i < 5; i++ {
		events, err := appendGoalEvent(meta, teamGoalEventInput{
			Type:    "progress",
			Message: "step",
		}, 3)
		if err != nil {
			t.Fatalf("append failed: %v", err)
		}
		if len(events) > 3 {
			t.Fatalf("expected cap 3, got %d", len(events))
		}
	}
	events, _ := meta["goal_events"].([]map[string]any)
	if len(events) != 3 {
		t.Fatalf("expected 3 events, got %d", len(events))
	}
}

func TestParseHandoffEvent(t *testing.T) {
	ev, err := parseHandoffEvent(map[string]any{
		"from_role": "Planner",
		"to_role":   "Executor",
		"reason":    "handoff",
	})
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if ev.FromRole != "planner" || ev.ToRole != "executor" {
		t.Fatalf("unexpected roles: %+v", ev)
	}

	if _, err := parseHandoffEvent(map[string]any{"from_role": "planner"}); err == nil {
		t.Fatal("expected error for missing to_role")
	}
}

func TestAppendHandoffEventCapped(t *testing.T) {
	meta := map[string]any{}
	for i := 0; i < 5; i++ {
		events, err := appendHandoffEvent(meta, teamHandoffEventInput{
			FromRole: "planner",
			ToRole:   "executor",
			Reason:   "step",
		}, 2)
		if err != nil {
			t.Fatalf("append failed: %v", err)
		}
		if len(events) > 2 {
			t.Fatalf("expected cap 2, got %d", len(events))
		}
	}
	events, _ := meta["handoff_events"].([]map[string]any)
	if len(events) != 2 {
		t.Fatalf("expected 2 events, got %d", len(events))
	}
}
