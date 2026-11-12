package broker

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"sync/atomic"
	"testing"

	"agentd-broker/internal/auth"
	"agentd-broker/internal/db"
	"agentd-broker/internal/proto"
	"agentd-broker/internal/registry"
	"agentd-broker/internal/workledger"
)

func admitReceiptFixture(t *testing.T, database *db.DB, id string) *workledger.DispatchReceipt {
	t.Helper()
	binding, meta := seededLedgerGoal(t, database, id)
	s := &Server{cfg: Config{DB: database}}
	receipt, err := s.admitWorkLedgerTurn(context.Background(), "team1", meta, binding)
	if err != nil {
		t.Fatal(err)
	}
	return receipt
}
func TestWorkLedgerReceiptPostgresAtomicImmutableAndRestart(t *testing.T) {
	database := ledgerTestDatabase(t)
	ctx := context.Background()
	admission := admitReceiptFixture(t, database, "receipt1")
	pending, err := database.GetGoalDispatch(ctx, "team1", "receipt1", admission.DispatchID)
	if err != nil || pending.State != "admitted" || pending.TeamRunID != "" {
		t.Fatalf("admission not durable: %+v %v", pending, err)
	}
	wrong := *admission
	wrong.Digest = strings.Repeat("b", 64)
	if _, err := database.CreateTeamRunWithGoalReceipt(ctx, "tr_wrong", "team1", "running", "owner", []byte("{}"), &wrong); err == nil {
		t.Fatal("wrong provenance accepted")
	}
	if _, err := database.GetTeamRun(ctx, "team1", "tr_wrong"); !db.IsGoalDispatchMissing(err) {
		t.Fatalf("orphan team row after receipt rollback: %v", err)
	}
	cancelled, cancel := context.WithCancel(ctx)
	cancel()
	if _, err := database.CreateTeamRunWithGoalReceipt(cancelled, "tr_cancel", "team1", "running", "owner", []byte("{}"), admission); err == nil {
		t.Fatal("cancelled transaction committed")
	}
	if _, err := database.CreateTeamRunWithGoalReceipt(ctx, "tr_original", "team1", "running", "owner", []byte("{}"), admission); err != nil {
		t.Fatal(err)
	}
	if _, err := database.CreateTeamRunWithGoalReceipt(ctx, "tr_duplicate", "team1", "running", "owner", []byte("{}"), admission); err == nil {
		t.Fatal("receipt overwritten")
	}
	if _, err := database.GetTeamRun(ctx, "team1", "tr_duplicate"); !db.IsGoalDispatchMissing(err) {
		t.Fatal("duplicate transaction not rolled back")
	}
	if err := database.UpdateTeamRunPayload(ctx, "team1", "tr_original", []byte(`{"team":{"work_ledger_dispatch_id":"forged"}}`)); err != nil {
		t.Fatal(err)
	}
	row, _ := database.GetOrchestratorRun(ctx, "team1", "receipt1")
	meta := row.Meta()
	meta["work_ledger_runtime"] = map[string]any{"fake_receipt": "tr_forged"}
	if _, err := database.UpdateOrchestratorRun(ctx, "team1", "receipt1", db.OrchestratorRunUpdate{Meta: meta}); err != nil {
		t.Fatal(err)
	}
	reopened, err := db.Open(ctx, database.Pool.Config().ConnConfig.ConnString())
	if err != nil {
		t.Fatal(err)
	}
	defer reopened.Close()
	if err := reopened.Migrate(ctx); err != nil {
		t.Fatal(err)
	}
	saved, err := reopened.GetGoalDispatch(ctx, "team1", "receipt1", admission.DispatchID)
	if err != nil || saved.State != "recorded" || saved.TeamRunID != "tr_original" || saved.Binding != admission.Binding || saved.Digest != admission.Digest {
		t.Fatalf("durable receipt changed: %+v %v", saved, err)
	}
	if _, err := reopened.GetGoalDispatch(ctx, "otherteam", "receipt1", admission.DispatchID); !db.IsGoalDispatchMissing(err) {
		t.Fatal("receipt crossed team scope")
	}
	if _, err := reopened.GetGoalDispatch(ctx, "team1", "othergoal", admission.DispatchID); !db.IsGoalDispatchMissing(err) {
		t.Fatal("receipt crossed goal scope")
	}
}
func TestWorkLedgerReceiptPostgresConcurrentWritersOneRow(t *testing.T) {
	database := ledgerTestDatabase(t)
	ctx := context.Background()
	admission := admitReceiptFixture(t, database, "concurrent")
	var wins atomic.Int32
	var wg sync.WaitGroup
	for i := 0; i < 12; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			if _, err := database.CreateTeamRunWithGoalReceipt(ctx, fmt.Sprintf("tr_%d", i), "team1", "running", "owner", []byte("{}"), admission); err == nil {
				wins.Add(1)
			}
		}(i)
	}
	wg.Wait()
	var rows int
	if err := database.Pool.QueryRow(ctx, "SELECT count(*) FROM broker_team_runs").Scan(&rows); err != nil {
		t.Fatal(err)
	}
	if wins.Load() != 1 || rows != 1 {
		t.Fatalf("receipt writers=%d team rows=%d", wins.Load(), rows)
	}
}
func TestWorkLedgerReceiptPostgresNonceCannotBeResetAndReused(t *testing.T) {
	database := ledgerTestDatabase(t)
	ctx := context.Background()
	binding, payload := seededLedgerGoal(t, database, "nonce_reset")
	before, _ := database.GetOrchestratorRun(ctx, "team1", "nonce_reset")
	server := &Server{cfg: Config{DB: database}}
	if _, err := server.admitWorkLedgerTurn(ctx, "team1", payload, binding); err != nil {
		t.Fatal(err)
	}
	if _, err := database.UpdateOrchestratorRun(ctx, "team1", "nonce_reset", db.OrchestratorRunUpdate{Meta: before.Meta()}); err != nil {
		t.Fatal(err)
	}
	if _, err := server.admitWorkLedgerTurn(ctx, "team1", payload, binding); !errors.Is(err, db.ErrGoalDispatchConflict) {
		t.Fatalf("reused nonce accepted: %v", err)
	}
	after, _ := database.GetOrchestratorRun(ctx, "team1", "nonce_reset")
	runtime := after.Meta()["work_ledger_runtime"].(map[string]any)
	if runtime["dispatch"].(map[string]any)["state"] != "admitted" {
		t.Fatal("failed nonce transaction left partial control update")
	}
}
func TestWorkLedgerReceiptPostgresFullControlCAS(t *testing.T) {
	database := ledgerTestDatabase(t)
	ctx := context.Background()
	for _, kind := range []string{"goal", "contract", "roles"} {
		t.Run(kind, func(t *testing.T) {
			id := "cas_" + kind
			binding, _ := seededLedgerGoal(t, database, id)
			before, _ := database.GetOrchestratorRun(ctx, "team1", id)
			runtime := before.Meta()["work_ledger_runtime"].(map[string]any)
			dispatch := runtime["dispatch"].(map[string]any)
			receipt := workledger.DispatchReceipt{Schema: "agentd.goal-dispatch.v1", TeamID: "team1", OrchestratorRunID: id, DispatchID: "nonce_" + id, Binding: binding, LedgerID: "work_one", Revision: 1, Digest: strings.Repeat("a", 64), ControlDigest: dispatch["control_digest"].(string), State: "admitted"}
			change := db.OrchestratorRunUpdate{}
			switch kind {
			case "goal":
				v := "New goal"
				change.Goal = &v
			case "contract":
				change.GoalContract = map[string]any{"new": true}
			case "roles":
				change.RolePlanSnapshot = map[string]any{"new": true}
			}
			if _, err := database.UpdateOrchestratorRun(ctx, "team1", id, change); err != nil {
				t.Fatal(err)
			}
			if _, err := database.AdmitGoalDispatch(ctx, before, before.Meta(), receipt); !errors.Is(err, db.ErrGoalDispatchConflict) {
				t.Fatalf("changed control escaped CAS: %v", err)
			}
			if _, err := database.GetGoalDispatch(ctx, "team1", id, receipt.DispatchID); !db.IsGoalDispatchMissing(err) {
				t.Fatal("conflicted admission persisted")
			}
		})
	}
}

type goalReceiptConn struct {
	*fakeSessionAliasConn
	calls         atomic.Int32
	afterDispatch func()
}

func (f *goalReceiptConn) WriteJSON(v any) error {
	req, ok := v.(proto.RelayRequest)
	if !ok {
		return errors.New("unexpected transport request")
	}
	if req.Req.Path != "/api/v1/run_async" {
		return errors.New("unexpected host path")
	}
	f.calls.Add(1)
	if f.afterDispatch != nil {
		f.afterDispatch()
	}
	f.agent.Deliver(proto.RelayResponse{Type: proto.TypeHTTPResp, ID: req.ID, Resp: proto.HTTPResponse{Status: 200, Headers: map[string]string{"Content-Type": "application/json"}, BodyB64: base64.StdEncoding.EncodeToString([]byte(`{"ok":true,"job_id":"original_host_job"}`))}})
	return nil
}

type lostGoalResponse struct{ header http.Header }

func (w *lostGoalResponse) Header() http.Header       { return w.header }
func (w *lostGoalResponse) WriteHeader(int)           {}
func (w *lostGoalResponse) Write([]byte) (int, error) { return 0, errors.New("client connection lost") }

func TestWorkLedgerReceiptPostgresActualAsyncLostClient(t *testing.T) {
	database := ledgerTestDatabase(t)
	ctx := context.Background()
	binding, meta := seededLedgerGoal(t, database, "http_goal")
	_, err := database.Pool.Exec(ctx, `INSERT INTO broker_users(sub) VALUES('client:test-client');
 INSERT INTO broker_agents(agent_id,owner_sub) VALUES('agent1','owner');
 INSERT INTO broker_team_members(member_id,team_id,agent_id,deployment_id,role) VALUES('member1','team1','agent1','dep1','worker')`)
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
	defer agent.Close()
	requestCtx, cancel := context.WithCancel(ctx)
	defer cancel()
	fake.afterDispatch = cancel // lose the client after host acceptance, before DB persistence
	meta["work_ledger"] = binding.Map()
	meta["mode"] = "async"
	meta["auto_allocate_roles"] = false
	payload, _ := json.Marshal(map[string]any{"run": map[string]any{"prompt": "Do verified work", "session_id": "session1", "no_session": false}, "team": meta})
	req := httptest.NewRequest("POST", "/v1/teams/team1/runs", strings.NewReader(string(payload))).WithContext(requestCtx)
	req.Header.Set("Authorization", "Bearer fixture")
	server.handleTeamsSubroutes(&lostGoalResponse{header: http.Header{}}, req)
	receipt, err := database.GetGoalDispatch(ctx, "team1", "http_goal", "nonce_http_goal")
	if err != nil || receipt.State != "recorded" || fake.calls.Load() != 1 {
		t.Fatalf("lost client discarded receipt: %+v %v, calls=%d", receipt, err, fake.calls.Load())
	}
	run, err := database.GetTeamRun(ctx, "team1", receipt.TeamRunID)
	if err != nil || !strings.Contains(string(run.RunJSON), "original_host_job") {
		t.Fatalf("original host job identity missing: %+v %v", run, err)
	}
	// Replay is rejected before host dispatch, even though the first response vanished.
	replay := httptest.NewRequest("POST", "/v1/teams/team1/runs", strings.NewReader(string(payload)))
	replay.Header.Set("Authorization", "Bearer fixture")
	writer := httptest.NewRecorder()
	server.handleTeamsSubroutes(writer, replay)
	if writer.Code != 409 || fake.calls.Load() != 1 {
		t.Fatalf("nonce replay reached host: %d %d %s", writer.Code, fake.calls.Load(), writer.Body.String())
	}
	// A fresh server object reads only the immutable DB record; no runtime cache.
	fresh := newTestAudioServer("fixture")
	fresh.cfg.DB = database
	fresh.cfg.ClientAuthAllowAutomation = true
	path := "/v1/teams/team1/orchestrator/runs/http_goal/dispatches/nonce_http_goal"
	get := httptest.NewRequest("GET", path, nil)
	get.Header.Set("Authorization", "Bearer fixture")
	writer = httptest.NewRecorder()
	fresh.handleTeamsSubroutes(writer, get)
	var response struct {
		OK       bool                       `json:"ok"`
		Dispatch workledger.DispatchReceipt `json:"dispatch"`
	}
	if err := json.Unmarshal(writer.Body.Bytes(), &response); err != nil || writer.Code != 200 || !response.OK || response.Dispatch.TeamRunID != receipt.TeamRunID || writer.Header().Get("Cache-Control") != "no-store" {
		t.Fatalf("original receipt not served: %d %s", writer.Code, writer.Body.String())
	}
	for _, tc := range []struct {
		method, path, token string
		code                int
	}{
		{"GET", path, "", 401}, {"POST", path, "fixture", 405}, {"GET", strings.Replace(path, "team1", "otherteam", 1), "fixture", 404},
		{"GET", path + "_absent", "fixture", 404}, {"GET", path + "/unexpected", "fixture", 400},
	} {
		q := httptest.NewRequest(tc.method, tc.path, nil)
		if tc.token != "" {
			q.Header.Set("Authorization", "Bearer "+tc.token)
		}
		w := httptest.NewRecorder()
		fresh.handleTeamsSubroutes(w, q)
		if w.Code != tc.code {
			t.Errorf("%s %s: got %d want %d", tc.method, tc.path, w.Code, tc.code)
		}
	}
	fresh.cfg.ClientAuthAllowAutomation = false
	writer = httptest.NewRecorder()
	fresh.handleTeamsSubroutes(writer, get)
	if writer.Code != 403 {
		t.Fatal("automation gate bypassed")
	}
	fresh.cfg.ClientAuthAllowAutomation = true
	fresh.SetClientAuth(&auth.ClientAuth{ByToken: map[string]*auth.ClientPolicy{"ordinary": {ClientID: "ordinary", Token: "ordinary", Admin: false}}})
	get.Header.Set("Authorization", "Bearer ordinary")
	writer = httptest.NewRecorder()
	fresh.handleTeamsSubroutes(writer, get)
	if writer.Code != 403 {
		t.Fatal("non-admin client token gained automation access")
	}
	denied := httptest.NewRecorder()
	if _, ok := fresh.requireTeamOwner(denied, get, &Principal{Sub: "other", AuthKind: "oidc"}, "team1"); ok || denied.Code != 403 {
		t.Fatal("other user allowed to read owned goal")
	}
}
