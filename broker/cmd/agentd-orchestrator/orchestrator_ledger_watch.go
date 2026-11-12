package main

import (
	"agentd-broker/internal/workledger"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"sort"
	"strconv"
	"sync"
	"time"
)

type ledgerWatchSpec struct {
	Binding  workledger.Binding
	LedgerID string
	Revision int64
}

func (s ledgerWatchSpec) key() string { data, _ := json.Marshal(s); return string(data) }
func ledgerWatchForRun(run orchestratorRun, owner string) (ledgerWatchSpec, bool) {
	var spec ledgerWatchSpec
	if !ledgerManagedWait(run) || !ledgerControllable(run, owner) || asString(run.Meta["active_team_run_id"]) != "" {
		return spec, false
	}
	runtime := ledgerRuntime(run.Meta)
	reason := asString(runtime["reason"])
	if reason != "requirements_blocked" && reason != "no_requirement_progress" {
		return spec, false
	}
	dispatch := ledgerDispatch(runtime)
	if dispatch["state"] == "admitted" || dispatch["state"] == "submitted" {
		return spec, false
	}
	binding, err := workledger.ParseBinding(run.Meta["work_ledger"])
	pinned, pinErr := workledger.ParseBinding(runtime["binding"])
	revision, ok := asInt64(runtime["revision"])
	id := asString(runtime["ledger_id"])
	if err != nil || pinErr != nil || binding != pinned || !ok || revision < 1 || revision > 9007199254740991 || !workledger.ValidID(id) {
		return spec, false
	}
	return ledgerWatchSpec{Binding: binding, LedgerID: id, Revision: revision}, true
}

type ledgerWatchWorker struct {
	cancel context.CancelFunc
	done   chan struct{}
}
type ledgerWatchManager struct {
	ctx     context.Context
	client  *http.Client
	cfg     config
	limit   int
	wake    chan struct{}
	workers map[string]ledgerWatchWorker
	wg      sync.WaitGroup
}

func newLedgerWatchManager(ctx context.Context, client *http.Client, cfg config) *ledgerWatchManager {
	limit := cfg.ledgerWatchLimit
	if limit <= 0 {
		limit = 16
	}
	if limit > 64 {
		limit = 64
	}
	return &ledgerWatchManager{ctx: ctx, client: client, cfg: cfg, limit: limit, wake: make(chan struct{}, 1), workers: map[string]ledgerWatchWorker{}}
}

// Called only by the serial reconciliation loop. Completed hints are rearmed
// only on a regular poll or a freshly observed revision, preventing hint storms.
func (m *ledgerWatchManager) sync(desired map[string]ledgerWatchSpec, rearm bool) {
	keys := make([]string, 0, len(desired))
	for key := range desired {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	if len(keys) > m.limit {
		keys = keys[:m.limit]
	}
	selected := map[string]ledgerWatchSpec{}
	for _, key := range keys {
		selected[key] = desired[key]
	}
	for key, worker := range m.workers {
		_, keep := selected[key]
		finished := false
		select {
		case <-worker.done:
			finished = true
		default:
		}
		if !keep || (finished && rearm) {
			worker.cancel()
			delete(m.workers, key)
		}
	}
	for key, spec := range selected {
		if _, exists := m.workers[key]; exists {
			continue
		}
		ctx, cancel := context.WithCancel(m.ctx)
		done := make(chan struct{})
		m.workers[key] = ledgerWatchWorker{cancel: cancel, done: done}
		m.wg.Add(1)
		go func() {
			defer m.wg.Done()
			defer close(done)
			for {
				started := time.Now()
				changed, err := waitLedgerChange(ctx, m.client, m.cfg, spec)
				if ctx.Err() != nil {
					return
				}
				if changed {
					select {
					case m.wake <- struct{}{}:
					default:
					}
					return
				}
				delay := 250*time.Millisecond - time.Since(started)
				if err != nil {
					delay = m.cfg.pollInterval
					if delay < time.Second {
						delay = time.Second
					}
					if delay > 30*time.Second {
						delay = 30 * time.Second
					}
				}
				if delay > 0 {
					timer := time.NewTimer(delay)
					select {
					case <-ctx.Done():
						timer.Stop()
						return
					case <-timer.C:
					}
				}
			}
		}()
	}
}
func (m *ledgerWatchManager) close() {
	for _, worker := range m.workers {
		worker.cancel()
	}
	m.wg.Wait()
}

func waitLedgerChange(ctx context.Context, client *http.Client, cfg config, spec ledgerWatchSpec) (bool, error) {
	// This is a scheduling hint only. Every wake goes through a fresh goal/ledger
	// read, owner controls, evidence validation, policy and existing admission CAS.
	requestCtx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	query := url.Values{"session_id": {spec.Binding.SessionID}, "after_ledger_id": {spec.LedgerID}, "after_revision": {strconv.FormatInt(spec.Revision, 10)}, "wait_ms": {"4000"}}
	endpoint := cfg.brokerBase + "/v1/agents/" + url.PathEscape(spec.Binding.AgentID) + "/proxy/api/v1/session/work?" + query.Encode()
	req, err := http.NewRequestWithContext(requestCtx, http.MethodGet, endpoint, nil)
	if err != nil {
		return false, err
	}
	token, err := cfg.bearerToken()
	if err != nil {
		return false, err
	}
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Accept", "application/json")
	req.Header.Set("X-Agentd-Deployment", spec.Binding.DeploymentID)
	guarded := *client
	guarded.CheckRedirect = func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }
	response, err := guarded.Do(req)
	if err != nil {
		return false, fmt.Errorf("ledger watch unavailable")
	}
	defer response.Body.Close()
	if response.StatusCode == http.StatusNoContent {
		return false, nil
	}
	if response.StatusCode != http.StatusOK {
		return false, fmt.Errorf("ledger watch returned HTTP %d", response.StatusCode)
	}
	const max = 2 * 1024 * 1024
	data, err := io.ReadAll(io.LimitReader(response.Body, max+1))
	if err != nil || len(data) > max {
		return false, fmt.Errorf("ledger watch response unavailable or too large")
	}
	var hint struct {
		OK        bool   `json:"ok"`
		SessionID string `json:"session_id"`
		State     struct {
			Schema   string `json:"schema"`
			LedgerID string `json:"ledger_id"`
			Revision int64  `json:"revision"`
		} `json:"state"`
		Watch struct {
			Schema  string `json:"schema"`
			Changed bool   `json:"changed"`
		} `json:"work_watch"`
	}
	if err := json.Unmarshal(data, &hint); err != nil || !hint.OK || hint.SessionID != spec.Binding.SessionID ||
		hint.Watch.Schema != "agentd.work-watch.v1" || !hint.Watch.Changed || hint.State.Schema != "agentd.work.v1" ||
		hint.State.Revision < 0 || hint.State.Revision > 9007199254740991 ||
		(hint.State.LedgerID != "" && !workledger.ValidID(hint.State.LedgerID)) ||
		(hint.State.LedgerID == spec.LedgerID && hint.State.Revision == spec.Revision) {
		return false, fmt.Errorf("invalid or unsupported ledger watch response")
	}
	return true, nil
}
