package workledger

import (
	"strings"
	"testing"
)

func TestDispatchReceiptValidation(t *testing.T) {
	good := DispatchReceipt{Schema: "agentd.goal-dispatch.v1", TeamID: "team", OrchestratorRunID: "goal", DispatchID: "nonce", Binding: Binding{AgentID: "agent", DeploymentID: "deployment", SessionID: "session"}, LedgerID: "work_one", Revision: 1, Digest: strings.Repeat("a", 64), ControlDigest: strings.Repeat("b", 64), State: "admitted", AdmittedUnixMS: 1000}
	if err := good.Validate(); err != nil {
		t.Fatal(err)
	}
	for _, mutate := range []func(*DispatchReceipt){
		func(r *DispatchReceipt) { r.Schema = "other" }, func(r *DispatchReceipt) { r.Binding.SessionID = "../secret" },
		func(r *DispatchReceipt) { r.Revision = 0 }, func(r *DispatchReceipt) { r.Digest = strings.Repeat("A", 64) },
		func(r *DispatchReceipt) { r.ControlDigest = "missing" }, func(r *DispatchReceipt) { r.TeamRunID = "unexpected" },
		func(r *DispatchReceipt) { r.State = "succeeded" }, func(r *DispatchReceipt) { r.RecordedUnixMS = 2000 },
	} {
		bad := good
		mutate(&bad)
		if bad.Validate() == nil {
			t.Fatalf("invalid receipt accepted: %+v", bad)
		}
	}
	good.State = "recorded"
	good.TeamRunID = "team_run"
	good.RecordedUnixMS = 2000
	if err := good.Validate(); err != nil {
		t.Fatal(err)
	}
	good.RecordedUnixMS = 999
	if good.Validate() == nil {
		t.Fatal("time-reversed receipt accepted")
	}
}
