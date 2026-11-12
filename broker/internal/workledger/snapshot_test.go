package workledger

import (
	"encoding/json"
	"strings"
	"testing"
)

func sample() Snapshot {
	return Snapshot{OK: true, SessionID: "s", State: State{Schema: "agentd.work.v1", LedgerID: "work_1", Revision: 1, Requirements: map[string]Requirement{"a": {Title: "A", Status: "pending", DependsOn: []string{}}, "b": {Title: "B", Status: "pending", DependsOn: []string{"a"}}}, Blockers: map[string]Blocker{}}}
}
func TestSnapshotAuthorityAndEvidenceValidation(t *testing.T) {
	binding := Binding{AgentID: "a", DeploymentID: "d", SessionID: "s"}
	for _, kind := range []string{"wrong_session", "empty", "legacy", "rollback", "oversized_revision", "missing_evidence", "bad_dependency", "cycle", "blocked_done", "bad_status", "nil_dependencies", "too_many_requirements"} {
		t.Run(kind, func(t *testing.T) {
			s := sample()
			switch kind {
			case "wrong_session":
				s.SessionID = "other"
			case "empty":
				s.State.Requirements = map[string]Requirement{}
			case "legacy":
				s.State.LedgerID = ""
			case "rollback":
				s.State.Revision = -1
			case "oversized_revision":
				s.State.Revision = 9007199254740992
			case "missing_evidence":
				r := s.State.Requirements["a"]
				r.Status = "done"
				s.State.Requirements["a"] = r
			case "bad_dependency":
				r := s.State.Requirements["b"]
				r.DependsOn = []string{"unknown"}
				s.State.Requirements["b"] = r
			case "cycle":
				r := s.State.Requirements["a"]
				r.DependsOn = []string{"b"}
				s.State.Requirements["a"] = r
			case "blocked_done":
				r := s.State.Requirements["a"]
				r.Status = "done"
				r.Evidence = "claim"
				s.State.Requirements["a"] = r
				s.State.Blockers["hold"] = Blocker{Status: "open", RequirementIDs: []string{"a"}}
			case "bad_status":
				r := s.State.Requirements["a"]
				r.Status = "success"
				s.State.Requirements["a"] = r
			case "nil_dependencies":
				r := s.State.Requirements["a"]
				r.DependsOn = nil
				s.State.Requirements["a"] = r
			case "too_many_requirements":
				for i := 0; i < 257; i++ {
					s.State.Requirements[strings.Repeat("x", i+1)] = Requirement{Title: "x", Status: "pending", DependsOn: []string{}}
				}
			}
			if s.Validate(binding) == nil {
				t.Fatal("invalid authority/requirement state accepted")
			}
		})
	}
	s := sample()
	if err := s.Validate(binding); err != nil {
		t.Fatal(err)
	}
	if len(s.Runnable) != 1 || s.Runnable[0] != "a" || s.AllDone {
		t.Fatal("invalid projection")
	}
	// Caller-supplied convenience booleans cannot override the actual ledger.
	raw, _ := json.Marshal(s)
	var m map[string]any
	_ = json.Unmarshal(raw, &m)
	m["all_requirements_done"] = true
	m["can_continue"] = false
	raw, _ = json.Marshal(m)
	var decoded Snapshot
	_ = json.Unmarshal(raw, &decoded)
	if err := decoded.Validate(binding); err != nil || decoded.AllDone || len(decoded.Runnable) != 1 {
		t.Fatal("trusted false convenience projection")
	}
}
func TestResolvedBlockerNeedsRecordedEvidence(t *testing.T) {
	s := sample()
	b := Blocker{Status: "resolved", RequirementIDs: []string{"a"}, Reason: "Access", RequiredAction: "Grant", Attempts: []string{"Denied"}}
	s.State.Blockers["access"] = b
	binding := Binding{AgentID: "a", DeploymentID: "d", SessionID: "s"}
	if s.Validate(binding) == nil {
		t.Fatal("resolution without evidence accepted")
	}
	b.Evidence = "Access verified"
	s.State.Blockers["access"] = b
	if err := s.Validate(binding); err != nil {
		t.Fatal(err)
	}
}

func TestBindingAndControlDigest(t *testing.T) {
	for _, value := range []any{nil, "https://other", map[string]any{"agent_id": "a", "deployment_id": "d", "session_id": "../s"}, map[string]any{"agent_id": "a", "deployment_id": "", "session_id": "s"}, map[string]any{"agent_id": "a", "deployment_id": "d", "session_id": "s", "url": "http://other"}} {
		if _, err := ParseBinding(value); err == nil {
			t.Fatal("invalid binding accepted")
		}
	}
	meta := map[string]any{"orchestrator_owner": "one"}
	first := ControlDigest("goal", nil, nil, meta)
	meta["work_ledger_runtime"] = map[string]any{"revision": 4}
	if ControlDigest("goal", map[string]any{}, map[string]any{}, meta) != first {
		t.Fatal("observation changed controls")
	}
	meta["orchestrator_owner"] = "two"
	if ControlDigest("goal", nil, nil, meta) == first {
		t.Fatal("owner change did not fence admission")
	}
}
