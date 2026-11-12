package main

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"sync/atomic"
	"testing"
	"time"
)

func eventuallyWatch(t *testing.T, condition func() bool) {
	t.Helper()
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		if condition() {
			return
		}
		time.Sleep(5 * time.Millisecond)
	}
	t.Fatal("watch condition did not become true within bounded test deadline")
}
func TestLedgerWatchManagerBoundsAndCancellation(t *testing.T) {
	var active, calls atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		active.Add(1)
		calls.Add(1)
		defer active.Add(-1)
		<-r.Context().Done()
	}))
	defer server.Close()
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	manager := newLedgerWatchManager(ctx, server.Client(), config{brokerBase: server.URL, oidcToken: "fixture", ledgerWatchLimit: 2})
	defer manager.close()
	desired := map[string]ledgerWatchSpec{}
	for i := 0; i < 3; i++ {
		spec := watchTestSpec()
		spec.Binding.SessionID = fmt.Sprintf("session%d", i)
		desired[spec.key()] = spec
	}
	manager.sync(desired, true)
	eventuallyWatch(t, func() bool { return active.Load() == 2 })
	time.Sleep(40 * time.Millisecond)
	if calls.Load() != 2 || len(manager.workers) != 2 {
		t.Fatal("watch capacity exceeded")
	}
	manager.sync(map[string]ledgerWatchSpec{}, true)
	eventuallyWatch(t, func() bool { return active.Load() == 0 })
	manager.sync(desired, true)
	eventuallyWatch(t, func() bool { return active.Load() == 2 })
	cancel()
	manager.close()
	eventuallyWatch(t, func() bool { return active.Load() == 0 })
}
func TestLedgerWatchHintsCoalesceAndRearmOnlyAfterPoll(t *testing.T) {
	var calls atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls.Add(1)
		_ = json.NewEncoder(w).Encode(watchTestHint(watchTestSpec()))
	}))
	defer server.Close()
	manager := newLedgerWatchManager(context.Background(), server.Client(), config{brokerBase: server.URL, oidcToken: "fixture"})
	defer manager.close()
	spec := watchTestSpec()
	desired := map[string]ledgerWatchSpec{spec.key(): spec}
	manager.sync(desired, true)
	eventuallyWatch(t, func() bool {
		select {
		case <-manager.workers[spec.key()].done:
			return true
		default:
			return false
		}
	})
	for i := 0; i < 5; i++ {
		manager.sync(desired, false)
	}
	time.Sleep(40 * time.Millisecond)
	if calls.Load() != 1 || len(manager.wake) != 1 {
		t.Fatal("same stale hint caused a request storm")
	}
	manager.sync(desired, true)
	eventuallyWatch(t, func() bool { return calls.Load() == 2 })
	eventuallyWatch(t, func() bool {
		select {
		case <-manager.workers[spec.key()].done:
			return true
		default:
			return false
		}
	})
	if len(manager.wake) != 1 {
		t.Fatal("wakes not coalesced")
	}
}
func TestLedgerWatchUnavailableBacksOffWithoutWake(t *testing.T) {
	var calls atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { calls.Add(1); w.WriteHeader(429) }))
	defer server.Close()
	manager := newLedgerWatchManager(context.Background(), server.Client(), config{brokerBase: server.URL, oidcToken: "fixture", pollInterval: time.Millisecond})
	defer manager.close()
	spec := watchTestSpec()
	manager.sync(map[string]ledgerWatchSpec{spec.key(): spec}, true)
	eventuallyWatch(t, func() bool { return calls.Load() == 1 })
	time.Sleep(100 * time.Millisecond)
	if calls.Load() != 1 || len(manager.wake) != 0 {
		t.Fatal("error spun or triggered an authoritative wake")
	}
}
func TestLedgerWatchImmediateUnchangedResponseIsThrottled(t *testing.T) {
	var calls atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { calls.Add(1); w.WriteHeader(204) }))
	defer server.Close()
	manager := newLedgerWatchManager(context.Background(), server.Client(), config{brokerBase: server.URL, oidcToken: "fixture"})
	defer manager.close()
	spec := watchTestSpec()
	manager.sync(map[string]ledgerWatchSpec{spec.key(): spec}, true)
	eventuallyWatch(t, func() bool { return calls.Load() == 1 })
	time.Sleep(100 * time.Millisecond)
	if calls.Load() != 1 || len(manager.wake) != 0 {
		t.Fatal("unchanged immediate response spun")
	}
}
