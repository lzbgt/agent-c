package broker

import "testing"

func TestMapDiffKeys(t *testing.T) {
	prev := map[string]any{
		"a": 1,
		"b": 2,
		"c": map[string]any{"x": 1},
	}
	next := map[string]any{
		"b": 2,
		"c": map[string]any{"x": 2},
		"d": 4,
	}
	diff := mapDiffKeys(prev, next)
	if diff == nil {
		t.Fatalf("expected diff")
	}
	added := diff["added"].([]string)
	removed := diff["removed"].([]string)
	changed := diff["changed"].([]string)
	if len(added) != 1 || added[0] != "d" {
		t.Fatalf("unexpected added keys: %v", added)
	}
	if len(removed) != 1 || removed[0] != "a" {
		t.Fatalf("unexpected removed keys: %v", removed)
	}
	if len(changed) != 1 || changed[0] != "c" {
		t.Fatalf("unexpected changed keys: %v", changed)
	}
	if mapDiffKeys(prev, prev) != nil {
		t.Fatalf("expected nil diff for identical maps")
	}
}

func TestMapDiffKeysNilEmpty(t *testing.T) {
	if mapDiffKeys(nil, nil) != nil {
		t.Fatalf("expected nil diff for nil maps")
	}
	if mapDiffKeys(map[string]any{}, nil) != nil {
		t.Fatalf("expected nil diff for empty vs nil")
	}
	if mapDiffKeys(nil, map[string]any{}) != nil {
		t.Fatalf("expected nil diff for nil vs empty")
	}
	diff := mapDiffKeys(nil, map[string]any{"a": 1})
	if diff == nil {
		t.Fatalf("expected diff for nil vs non-empty")
	}
	added := diff["added"].([]string)
	if len(added) != 1 || added[0] != "a" {
		t.Fatalf("unexpected added keys: %v", added)
	}
}

func TestMapDiffKeysSorted(t *testing.T) {
	prev := map[string]any{
		"b": 1,
		"z": 1,
		"a": 1,
	}
	next := map[string]any{
		"a": 1,
		"c": 1,
		"y": 1,
	}
	diff := mapDiffKeys(prev, next)
	if diff == nil {
		t.Fatalf("expected diff")
	}
	added := diff["added"].([]string)
	removed := diff["removed"].([]string)
	if len(added) != 2 || added[0] != "c" || added[1] != "y" {
		t.Fatalf("unexpected added ordering: %v", added)
	}
	if len(removed) != 2 || removed[0] != "b" || removed[1] != "z" {
		t.Fatalf("unexpected removed ordering: %v", removed)
	}
}

func TestNextRevisionVersion(t *testing.T) {
	entries := []map[string]any{
		{"version": 1},
		{"version": "3"},
		{"version": float64(4)},
	}
	if got := nextRevisionVersion(entries); got != 5 {
		t.Fatalf("expected next version 5, got %d", got)
	}
	if got := nextRevisionVersion(nil); got != 1 {
		t.Fatalf("expected next version 1, got %d", got)
	}
}

func TestTrimRevisionEntries(t *testing.T) {
	entries := []map[string]any{
		{"version": 1},
		{"version": 2},
		{"version": 3},
	}
	if got := trimRevisionEntries(entries, 0); len(got) != 3 {
		t.Fatalf("expected no trim when max=0, got %d entries", len(got))
	}
	trimmed := trimRevisionEntries(entries, 2)
	if len(trimmed) != 2 {
		t.Fatalf("expected 2 entries, got %d", len(trimmed))
	}
	if trimmed[0]["version"] != 2 || trimmed[1]["version"] != 3 {
		t.Fatalf("unexpected trimmed entries: %#v", trimmed)
	}
}

func TestReadRevisionEntries(t *testing.T) {
	meta := map[string]any{
		"goal_versions": []any{
			map[string]any{"version": 1},
			"skip",
			map[string]any{"version": 2},
		},
	}
	entries := readRevisionEntries(meta, "goal_versions")
	if len(entries) != 2 {
		t.Fatalf("expected 2 entries, got %d", len(entries))
	}
	if v, ok := entries[0]["version"]; !ok || v != 1 {
		t.Fatalf("unexpected first entry: %v", entries[0])
	}
}

func TestMergeOrchestratorRevisionHistory(t *testing.T) {
	dst := map[string]any{
		"goal_version": 2,
	}
	src := map[string]any{
		"goal_version":      1,
		"role_plan_version": 3,
	}
	mergeOrchestratorRevisionHistory(dst, src)
	if dst["goal_version"] != 2 {
		t.Fatalf("expected goal_version to remain 2, got %v", dst["goal_version"])
	}
	if dst["role_plan_version"] != 3 {
		t.Fatalf("expected role_plan_version to be copied, got %v", dst["role_plan_version"])
	}
}

func TestInitializeOrchestratorRevisionHistory(t *testing.T) {
	meta := initializeOrchestratorRevisionHistory(nil, "goal", map[string]any{"scope": "s1"}, map[string]any{"role": "plan"}, "user1")
	goalVersions := readRevisionEntries(meta, "goal_versions")
	if len(goalVersions) != 1 {
		t.Fatalf("expected goal_versions seeded, got %d", len(goalVersions))
	}
	if meta["goal_version"] != int64(1) {
		t.Fatalf("expected goal_version=1, got %v", meta["goal_version"])
	}
	roleVersions := readRevisionEntries(meta, "role_plan_versions")
	if len(roleVersions) != 1 {
		t.Fatalf("expected role_plan_versions seeded, got %d", len(roleVersions))
	}
	if meta["role_plan_version"] != int64(1) {
		t.Fatalf("expected role_plan_version=1, got %v", meta["role_plan_version"])
	}
}

func TestInitializeOrchestratorRevisionHistoryNilContract(t *testing.T) {
	meta := initializeOrchestratorRevisionHistory(nil, "goal", nil, nil, "user1")
	goalVersions := readRevisionEntries(meta, "goal_versions")
	if len(goalVersions) != 1 {
		t.Fatalf("expected goal_versions seeded, got %d", len(goalVersions))
	}
	contract, ok := goalVersions[0]["goal_contract"].(map[string]any)
	if !ok || contract == nil {
		t.Fatalf("expected normalized goal_contract map, got %#v", goalVersions[0]["goal_contract"])
	}
	if len(contract) != 0 {
		t.Fatalf("expected empty goal_contract, got %#v", contract)
	}
	if len(readRevisionEntries(meta, "role_plan_versions")) != 0 {
		t.Fatalf("expected no role_plan_versions when snapshot is nil")
	}
}

func TestLatestRevisionEntry(t *testing.T) {
	entries := []map[string]any{
		{"version": 2},
		{"version": 1},
		{"note": "no version"},
		{"version": 5},
	}
	latest := latestRevisionEntry(entries)
	if latest == nil || latest["version"] != 5 {
		t.Fatalf("expected latest version 5, got %#v", latest)
	}
	entries = []map[string]any{{"note": "a"}, {"note": "b"}}
	latest = latestRevisionEntry(entries)
	if latest == nil || latest["note"] != "a" {
		t.Fatalf("expected first entry fallback, got %#v", latest)
	}
}

func TestBuildRevisionPayloads(t *testing.T) {
	meta := map[string]any{
		"goal_versions": []map[string]any{
			{"version": 1, "goal": "old"},
			{
				"version":               3,
				"goal":                  "new",
				"goal_changed":          true,
				"goal_contract_changed": true,
				"goal_contract":         map[string]any{"scope": "s1"},
				"goal_contract_diff":    map[string]any{"added": []string{"scope"}},
			},
			{"version": 2, "goal": "mid"},
		},
		"role_plan_versions": []map[string]any{
			{"version": 2, "role_plan_snapshot": map[string]any{"role": "b"}},
			{"version": 4, "role_plan_snapshot": map[string]any{"role": "c"}, "role_plan_diff": map[string]any{"changed": []string{"role"}}},
		},
	}
	goalPayload := buildGoalRevisionPayload("team1", "run1", meta)
	if goalPayload == nil || goalPayload["version"] != 3 {
		t.Fatalf("expected goal payload version 3, got %#v", goalPayload)
	}
	if goalPayload["goal"] != "new" {
		t.Fatalf("expected goal payload 'new', got %#v", goalPayload["goal"])
	}
	if goalPayload["goal_changed"] != true {
		t.Fatalf("expected goal_changed true, got %#v", goalPayload["goal_changed"])
	}
	if goalPayload["goal_contract_changed"] != true {
		t.Fatalf("expected goal_contract_changed true, got %#v", goalPayload["goal_contract_changed"])
	}
	goalDiff, ok := goalPayload["goal_contract_diff"].(map[string]any)
	if !ok {
		t.Fatalf("expected goal_contract_diff, got %#v", goalPayload["goal_contract_diff"])
	}
	added, ok := goalDiff["added"].([]string)
	if !ok || len(added) != 1 || added[0] != "scope" {
		t.Fatalf("unexpected goal_contract_diff added keys: %#v", goalDiff["added"])
	}
	rolePayload := buildRolePlanRevisionPayload("team1", "run1", meta)
	if rolePayload == nil || rolePayload["version"] != 4 {
		t.Fatalf("expected role payload version 4, got %#v", rolePayload)
	}
	if rolePayload["role_plan_snapshot"] == nil {
		t.Fatalf("expected role_plan_snapshot in payload")
	}
	roleDiff, ok := rolePayload["role_plan_diff"].(map[string]any)
	if !ok {
		t.Fatalf("expected role_plan_diff, got %#v", rolePayload["role_plan_diff"])
	}
	changed, ok := roleDiff["changed"].([]string)
	if !ok || len(changed) != 1 || changed[0] != "role" {
		t.Fatalf("unexpected role_plan_diff changed keys: %#v", roleDiff["changed"])
	}
}
