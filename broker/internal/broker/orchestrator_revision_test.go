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
			{"version": 3, "goal": "new", "goal_contract": map[string]any{"scope": "s1"}},
			{"version": 2, "goal": "mid"},
		},
		"role_plan_versions": []map[string]any{
			{"version": 2, "role_plan_snapshot": map[string]any{"role": "b"}},
			{"version": 4, "role_plan_snapshot": map[string]any{"role": "c"}},
		},
	}
	goalPayload := buildGoalRevisionPayload("team1", "run1", meta)
	if goalPayload == nil || goalPayload["version"] != 3 {
		t.Fatalf("expected goal payload version 3, got %#v", goalPayload)
	}
	if goalPayload["goal"] != "new" {
		t.Fatalf("expected goal payload 'new', got %#v", goalPayload["goal"])
	}
	rolePayload := buildRolePlanRevisionPayload("team1", "run1", meta)
	if rolePayload == nil || rolePayload["version"] != 4 {
		t.Fatalf("expected role payload version 4, got %#v", rolePayload)
	}
	if rolePayload["role_plan_snapshot"] == nil {
		t.Fatalf("expected role_plan_snapshot in payload")
	}
}
