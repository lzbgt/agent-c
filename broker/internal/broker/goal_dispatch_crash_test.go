package broker

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"agentd-broker/internal/db"
	"agentd-broker/internal/registry"
	"agentd-broker/internal/workledger"
)

type crashGoalResponse struct{ header http.Header }

func (w *crashGoalResponse) Header() http.Header       { return w.header }
func (w *crashGoalResponse) WriteHeader(int)           {}
func (w *crashGoalResponse) Write([]byte) (int, error) { os.Exit(77); return 0, nil }

// Invoked only as a child process against the parent's disposable test schema.
// os.Exit intentionally skips all broker defers, approximating an abrupt death.
func TestWorkLedgerReceiptCrashHelper(t *testing.T) {
	mode := os.Getenv("AGENT_GOAL_RECEIPT_CRASH_MODE")
	if mode == "" {
		return
	}
	database, err := db.Open(context.Background(), os.Getenv("AGENT_GOAL_RECEIPT_CRASH_DSN"))
	if err != nil {
		t.Fatal(err)
	}
	server := newTestAudioServer("fixture")
	server.cfg.DB = database
	server.cfg.ClientAuthAllowAutomation = true
	server.cfg.Registry = registry.New()
	agent := &registry.AgentConn{AgentID: "agent1", DeploymentID: "dep1"}
	agent.InitSession()
	fake := &goalReceiptConn{fakeSessionAliasConn: &fakeSessionAliasConn{agent: agent}}
	agent.Conn = fake
	server.cfg.Registry.Upsert(agent)
	fake.afterDispatch = func() {
		f, err := os.OpenFile(os.Getenv("AGENT_GOAL_RECEIPT_CRASH_CALLS"), os.O_WRONLY|os.O_CREATE|os.O_APPEND, 0600)
		if err != nil {
			t.Fatal(err)
		}
		if _, err = f.WriteString("original host acceptance\n"); err != nil {
			t.Fatal(err)
		}
		if err = f.Sync(); err != nil {
			t.Fatal(err)
		}
		f.Close()
		if mode == "before_receipt" {
			os.Exit(77)
		}
	}
	binding := workledger.Binding{AgentID: "agent1", DeploymentID: "dep1", SessionID: "session1"}
	body, _ := json.Marshal(map[string]any{"run": map[string]any{"prompt": "Complete verified work", "session_id": "session1"}, "team": map[string]any{
		"mode": "async", "auto_allocate_roles": false, "work_ledger": binding.Map(), "work_ledger_orchestrator_run_id": "crash_goal", "work_ledger_dispatch_id": "nonce_crash_goal",
	}})
	request := httptest.NewRequest("POST", "/v1/teams/team1/runs", strings.NewReader(string(body)))
	request.Header.Set("Authorization", "Bearer fixture")
	if mode == "after_receipt" {
		server.handleTeamsSubroutes(&crashGoalResponse{header: http.Header{}}, request)
		t.Fatal("expected crash on reply")
	}
	writer := httptest.NewRecorder()
	server.handleTeamsSubroutes(writer, request)
	if mode == "replay" {
		if writer.Code != 409 {
			t.Fatalf("restarted replay not rejected: %d %s", writer.Code, writer.Body.String())
		}
		return
	}
	t.Fatal("expected process interruption")
}
func TestWorkLedgerReceiptPostgresProcessCrashBoundaries(t *testing.T) {
	for _, boundary := range []string{"before_receipt", "after_receipt"} {
		t.Run(boundary, func(t *testing.T) {
			database := ledgerTestDatabase(t)
			ctx := context.Background()
			seededLedgerGoal(t, database, "crash_goal")
			_, err := database.Pool.Exec(ctx, `INSERT INTO broker_users(sub) VALUES('client:test-client');
 INSERT INTO broker_agents(agent_id,owner_sub) VALUES('agent1','owner');
 INSERT INTO broker_team_members(member_id,team_id,agent_id,deployment_id,role) VALUES('member1','team1','agent1','dep1','worker')`)
			if err != nil {
				t.Fatal(err)
			}
			calls := filepath.Join(t.TempDir(), "host_acceptances.txt")
			invoke := func(mode string) error {
				command := exec.Command(os.Args[0], "-test.run=^TestWorkLedgerReceiptCrashHelper$", "-test.v")
				command.Env = append(os.Environ(), "AGENT_GOAL_RECEIPT_CRASH_MODE="+mode, "AGENT_GOAL_RECEIPT_CRASH_DSN="+database.Pool.Config().ConnConfig.ConnString(), "AGENT_GOAL_RECEIPT_CRASH_CALLS="+calls)
				output, err := command.CombinedOutput()
				if mode == "replay" && err != nil {
					t.Log(string(output))
				}
				return err
			}
			err = invoke(boundary)
			if exit, ok := err.(*exec.ExitError); !ok || exit.ExitCode() != 77 {
				t.Fatalf("wrong crash outcome: %v", err)
			}
			receipt, err := database.GetGoalDispatch(ctx, "team1", "crash_goal", "nonce_crash_goal")
			if err != nil {
				t.Fatal(err)
			}
			var rows int
			if err := database.Pool.QueryRow(ctx, "SELECT count(*) FROM broker_team_runs").Scan(&rows); err != nil {
				t.Fatal(err)
			}
			if boundary == "before_receipt" {
				if receipt.State != "admitted" || receipt.TeamRunID != "" || rows != 0 {
					t.Fatalf("unknown outcome fabricated a receipt: %+v rows=%d", receipt, rows)
				}
			} else {
				if receipt.State != "recorded" || receipt.TeamRunID == "" || rows != 1 {
					t.Fatalf("committed receipt not durable: %+v rows=%d", receipt, rows)
				}
			}
			if err := invoke("replay"); err != nil {
				t.Fatal(err)
			}
			data, err := os.ReadFile(calls)
			if err != nil {
				t.Fatal(err)
			}
			if string(data) != "original host acceptance\n" {
				t.Fatal(fmt.Sprintf("restart resent host work: %q", data))
			}
		})
	}
}
