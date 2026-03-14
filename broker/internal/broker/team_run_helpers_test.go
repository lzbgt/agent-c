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

func TestParseGoalEventAllowsReplanResume(t *testing.T) {
	_, err := parseGoalEvent(map[string]any{
		"type":    "replan_resume",
		"message": "resume after replan",
		"data": map[string]any{
			"guidance_id": "g1",
		},
		"ts_unix_ms": int64(123),
	})
	if err != nil {
		t.Fatalf("expected replan_resume event to parse, got %v", err)
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

func TestCollectRolePlanRoles(t *testing.T) {
	meta := map[string]any{
		"role_graph": map[string]any{
			"edges": []any{
				map[string]any{"from_role": "Planner", "to_role": "Executor"},
				map[string]any{"from": "Reviewer", "to": "Planner"},
			},
		},
	}
	roleOverrides := map[string]map[string]any{"ops": {"model": "x"}, "planner": {"tools": "basic"}}
	roleInstructions := map[string]string{"writer": "Write it."}
	got := collectRolePlanRoles(meta, roleOverrides, roleInstructions)
	want := []string{"executor", "ops", "planner", "reviewer", "writer"}
	if len(got) != len(want) {
		t.Fatalf("expected %v, got %v", want, got)
	}
	for i, role := range want {
		if got[i] != role {
			t.Fatalf("expected %v, got %v", want, got)
		}
	}
}

func TestAsBool(t *testing.T) {
	if v, ok := asBool(true); !ok || !v {
		t.Fatalf("expected true from bool")
	}
	if v, ok := asBool("yes"); !ok || !v {
		t.Fatalf("expected true from string")
	}
	if v, ok := asBool("0"); !ok || v {
		t.Fatalf("expected false from string")
	}
	if _, ok := asBool(123); ok {
		t.Fatalf("expected non-bool to be invalid")
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

func TestParseHandoffEventCrossDeploymentResolution(t *testing.T) {
	ev, err := parseHandoffEvent(map[string]any{
		"handoff_id": "th_existing",
		"kind":       "cross_deployment",
		"state":      "accepted",
		"message":    "target accepted",
	})
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if ev.HandoffID != "th_existing" || ev.Kind != "cross_deployment" || ev.State != "accepted" {
		t.Fatalf("unexpected handoff event: %+v", ev)
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

func TestAppendHandoffEventHydratesCrossDeploymentResolution(t *testing.T) {
	meta := map[string]any{}
	events, err := appendHandoffEvent(meta, teamHandoffEventInput{
		Kind:               "cross_deployment",
		FromRole:           "planner",
		ToRole:             "executor",
		SourceDeploymentID: "dep-a",
		SourceSessionID:    "sess-a",
		TargetDeploymentID: "dep-b",
		TargetSessionID:    "sess-b",
		Message:            "please continue there",
	}, 10)
	if err != nil {
		t.Fatalf("proposal append failed: %v", err)
	}
	if len(events) != 1 {
		t.Fatalf("expected 1 event, got %d", len(events))
	}
	handoffID, _ := events[0]["handoff_id"].(string)
	if handoffID == "" {
		t.Fatal("expected generated handoff_id")
	}
	events, err = appendHandoffEvent(meta, teamHandoffEventInput{
		HandoffID: handoffID,
		Kind:      "cross_deployment",
		State:     "accepted",
		Message:   "accepted",
	}, 10)
	if err != nil {
		t.Fatalf("resolution append failed: %v", err)
	}
	if len(events) != 2 {
		t.Fatalf("expected 2 events, got %d", len(events))
	}
	last := events[len(events)-1]
	if last["handoff_id"] != handoffID {
		t.Fatalf("expected handoff_id %q, got %#v", handoffID, last["handoff_id"])
	}
	if last["state"] != "accepted" {
		t.Fatalf("expected accepted state, got %#v", last["state"])
	}
	if last["source_deployment_id"] != "dep-a" || last["target_deployment_id"] != "dep-b" {
		t.Fatalf("expected deployment hydration, got %#v", last)
	}
	if last["source_session_id"] != "sess-a" || last["target_session_id"] != "sess-b" {
		t.Fatalf("expected session hydration, got %#v", last)
	}
	if last["from_role"] != "planner" || last["to_role"] != "executor" {
		t.Fatalf("expected role hydration, got %#v", last)
	}
}
