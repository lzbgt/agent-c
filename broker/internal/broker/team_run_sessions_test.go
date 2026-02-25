package broker

import "testing"

func TestIsSessionIDSafe(t *testing.T) {
	if !isSessionIDSafe("team_run_member-1") {
		t.Fatalf("expected safe session_id")
	}
	if isSessionIDSafe("") {
		t.Fatalf("expected empty session_id to be unsafe")
	}
	if isSessionIDSafe("bad/seg") {
		t.Fatalf("expected slash to be unsafe")
	}
	if isSessionIDSafe("..") {
		t.Fatalf("expected .. to be unsafe")
	}
	if isSessionIDSafe("bad..seg") {
		t.Fatalf("expected .. substring to be unsafe")
	}
	if isSessionIDSafe("bad space") {
		t.Fatalf("expected spaces to be unsafe")
	}
}

func TestMakeTeamRunSessionID(t *testing.T) {
	id := makeTeamRunSessionID("team-1", "tr_123", "member-1")
	if !isSessionIDSafe(id) {
		t.Fatalf("expected generated session_id to be safe, got %q", id)
	}
	id2 := makeTeamRunSessionID("team-1", "tr_123", "member-1")
	if id != id2 {
		t.Fatalf("expected deterministic session_id, got %q vs %q", id, id2)
	}
}

func TestTeamRunMemberSessionsFromMeta(t *testing.T) {
	meta := map[string]any{
		"member_sessions": map[string]any{
			"m1": "sess-1",
			"m2": "sess-2",
		},
	}
	out := teamRunMemberSessionsFromMeta(meta)
	if len(out) != 2 {
		t.Fatalf("expected 2 sessions, got %d", len(out))
	}
	if out["m1"] != "sess-1" {
		t.Fatalf("expected m1 sess-1, got %q", out["m1"])
	}
}
