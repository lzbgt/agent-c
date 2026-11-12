package broker

import (
	"agentd-broker/internal/db"
	"agentd-broker/internal/workledger"
	"context"
	"fmt"
	"net/url"
	"os"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

func ledgerTestDatabase(t *testing.T) *db.DB {
	t.Helper()
	dsn := os.Getenv("AGENT_TEST_POSTGRES_DSN")
	if dsn == "" {
		t.Skip("AGENT_TEST_POSTGRES_DSN not set; isolated PostgreSQL integration only")
	}
	ctx := context.Background()
	admin, err := db.Open(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	schema := fmt.Sprintf("ledger_test_%d", time.Now().UnixNano())
	if _, err = admin.Pool.Exec(ctx, "CREATE SCHEMA "+schema); err != nil {
		admin.Close()
		t.Fatal(err)
	}
	t.Cleanup(func() { _, _ = admin.Pool.Exec(context.Background(), "DROP SCHEMA "+schema+" CASCADE"); admin.Close() })
	parsed, err := url.Parse(dsn)
	if err != nil {
		t.Fatal(err)
	}
	q := parsed.Query()
	q.Set("search_path", schema)
	parsed.RawQuery = q.Encode()
	database, err := db.Open(ctx, parsed.String())
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(database.Close)
	_, err = database.Pool.Exec(ctx, `CREATE TABLE broker_orchestrator_runs(
 orchestrator_run_id TEXT PRIMARY KEY,team_id TEXT NOT NULL,status TEXT NOT NULL,goal TEXT,
 goal_contract JSONB NOT NULL DEFAULT '{}',role_plan_snapshot JSONB NOT NULL DEFAULT '{}',meta JSONB NOT NULL DEFAULT '{}',
 created_by TEXT,created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),last_heartbeat_at TIMESTAMPTZ)`)
	if err != nil {
		t.Fatal(err)
	}
	return database
}
func seededLedgerGoal(t *testing.T, database *db.DB, id string) (workledger.Binding, map[string]any) {
	t.Helper()
	binding := workledger.Binding{AgentID: "agent1", DeploymentID: "dep1", SessionID: "session1"}
	meta := map[string]any{"orchestrator_owner": "worker", "work_ledger": binding.Map()}
	meta["work_ledger_runtime"] = map[string]any{"dispatch": map[string]any{"id": "nonce_" + id, "state": "admitted", "control_digest": workledger.ControlDigest("Goal", map[string]any{}, map[string]any{}, meta)}}
	_, err := database.CreateOrchestratorRun(context.Background(), id, "team1", "running", "Goal", "owner", nil, nil, meta)
	if err != nil {
		t.Fatal(err)
	}
	return binding, map[string]any{"work_ledger_orchestrator_run_id": id, "work_ledger_dispatch_id": "nonce_" + id}
}
func TestWorkLedgerAdmissionPostgresSingleUseAndCAS(t *testing.T) {
	database := ledgerTestDatabase(t)
	server := &Server{cfg: Config{DB: database}}
	binding, payload := seededLedgerGoal(t, database, "goal1")
	var wins atomic.Int32
	var wg sync.WaitGroup
	for i := 0; i < 12; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			if server.admitWorkLedgerTurn(context.Background(), "team1", payload, binding) == nil {
				wins.Add(1)
			}
		}()
	}
	wg.Wait()
	if wins.Load() != 1 {
		t.Fatalf("admission wins %d, want one", wins.Load())
	}
	if server.admitWorkLedgerTurn(context.Background(), "team1", payload, binding) == nil {
		t.Fatal("consumed nonce replayed")
	}
	row, err := database.GetOrchestratorRun(context.Background(), "team1", "goal1")
	if err != nil {
		t.Fatal(err)
	}
	original := row.Meta()
	next := cloneMap(original)
	next["owner_note"] = "new instruction"
	_, err = database.UpdateOrchestratorRun(context.Background(), "team1", "goal1", db.OrchestratorRunUpdate{Meta: next, ExpectedMeta: original})
	if err != nil {
		t.Fatal(err)
	}
	_, err = database.UpdateOrchestratorRun(context.Background(), "team1", "goal1", db.OrchestratorRunUpdate{Meta: original, ExpectedMeta: original})
	if err != db.ErrOrchestratorRunConflict {
		t.Fatalf("stale JSONB CAS accepted: %v", err)
	}
}
func TestWorkLedgerAdmissionPostgresHonorsLaterOwnerControls(t *testing.T) {
	database := ledgerTestDatabase(t)
	server := &Server{cfg: Config{DB: database}}
	for _, change := range []string{"pause", "owner", "template", "goal", "binding", "autonomous"} {
		t.Run(change, func(t *testing.T) {
			id := "goal_" + change
			binding, payload := seededLedgerGoal(t, database, id)
			row, _ := database.GetOrchestratorRun(context.Background(), "team1", id)
			meta := row.Meta()
			update := db.OrchestratorRunUpdate{}
			switch change {
			case "pause":
				value := "paused"
				update.Status = &value
			case "owner":
				meta["orchestrator_owner"] = "new_owner"
				update.Meta = meta
			case "template":
				meta["run_template"] = map[string]any{"prompt": "New owner instruction"}
				update.Meta = meta
			case "goal":
				value := "New goal"
				update.Goal = &value
			case "binding":
				meta["work_ledger"] = map[string]any{"agent_id": "other", "deployment_id": "dep1", "session_id": "session1"}
				update.Meta = meta
			case "autonomous":
				meta["autonomous"] = false
				update.Meta = meta
			}
			if _, err := database.UpdateOrchestratorRun(context.Background(), "team1", id, update); err != nil {
				t.Fatal(err)
			}
			if server.admitWorkLedgerTurn(context.Background(), "team1", payload, binding) == nil {
				t.Fatal("later owner control ignored")
			}
		})
	}
}
func TestWorkLedgerManualWaitClearsOnlyManagedWake(t *testing.T) {
	meta := map[string]any{"work_ledger_runtime": map[string]any{"managed_wait": true, "ledger_id": "work_one", "dispatch": map[string]any{"id": "nonce"}}}
	next := manualWorkLedgerWaitMeta(meta)
	runtime := next["work_ledger_runtime"].(map[string]any)
	if runtime["managed_wait"] != false || runtime["reason"] != "owner_waiting" || runtime["ledger_id"] != "work_one" {
		t.Fatal("manual wait did not preserve identity and stop wakeup")
	}
	if meta["work_ledger_runtime"].(map[string]any)["managed_wait"] != true {
		t.Fatal("mutated CAS source snapshot")
	}
}

func TestWorkLedgerAuthoritySelection(t *testing.T) {
	b := workledger.Binding{AgentID: "a", DeploymentID: "d", SessionID: "s"}
	member := db.TeamMember{MemberID: "one", AgentID: "a", DeploymentID: "d"}
	if got, err := selectWorkLedgerAuthority([]db.TeamMember{member, {MemberID: "other", AgentID: "b", DeploymentID: "d"}}, b); err != nil || len(got) != 1 {
		t.Fatal("exact authority not selected")
	}
	for _, members := range [][]db.TeamMember{{}, {member, member}, {{MemberID: "wrong", AgentID: "a", DeploymentID: "other"}}} {
		if _, err := selectWorkLedgerAuthority(members, b); err == nil {
			t.Fatal("missing, duplicate or retargeted authority accepted")
		}
	}
}
