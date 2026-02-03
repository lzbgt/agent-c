package registry

import (
	"testing"
	"time"

	"agentd-broker/internal/proto"
)

func TestAgentConn_UnregisterPendingCloses(t *testing.T) {
	a := &AgentConn{AgentID: "a1"}
	a.InitSession()

	ch, err := a.RegisterPending("req1")
	if err != nil {
		t.Fatalf("RegisterPending: %v", err)
	}

	a.UnregisterPending("req1")

	select {
	case <-ch:
		// either a value or closed; both indicate no leak.
	case <-time.After(250 * time.Millisecond):
		t.Fatalf("expected pending channel to be closed promptly")
	}
}

func TestAgentConn_PendingLimit(t *testing.T) {
	a := &AgentConn{AgentID: "a1", PendingLimit: 1}
	a.InitSession()

	if _, err := a.RegisterPending("req1"); err != nil {
		t.Fatalf("RegisterPending(1): %v", err)
	}
	if _, err := a.RegisterPending("req2"); err == nil {
		t.Fatalf("expected error when exceeding pending limit")
	}
}

func TestAgentConn_DeliverDoesNotBlockIfBufferFull(t *testing.T) {
	a := &AgentConn{AgentID: "a1"}
	a.InitSession()

	ch, err := a.RegisterPending("req1")
	if err != nil {
		t.Fatalf("RegisterPending: %v", err)
	}
	// Fill the buffered channel to simulate a client that stopped receiving.
	ch <- proto.RelayResponse{Type: proto.TypeHTTPResp, ID: "req1", Err: "pre-filled"}

	done := make(chan struct{})
	go func() {
		a.Deliver(proto.RelayResponse{Type: proto.TypeHTTPResp, ID: "req1", Err: "late"})
		close(done)
	}()

	select {
	case <-done:
	case <-time.After(250 * time.Millisecond):
		t.Fatalf("Deliver blocked; should be non-blocking under backpressure")
	}
}

func TestAgentConn_StreamLimit(t *testing.T) {
	a := &AgentConn{AgentID: "a1", StreamLimit: 1}
	a.InitSession()

	if _, err := a.RegisterStream("s1"); err != nil {
		t.Fatalf("RegisterStream(1): %v", err)
	}
	if _, err := a.RegisterStream("s2"); err == nil {
		t.Fatalf("expected error when exceeding stream limit")
	}
}

func TestAgentConn_DeliverStreamClosesOnBackpressure(t *testing.T) {
	a := &AgentConn{AgentID: "a1"}
	a.InitSession()

	ch, err := a.RegisterStream("s1")
	if err != nil {
		t.Fatalf("RegisterStream: %v", err)
	}
	// Buffer is size 16; fill it.
	for i := 0; i < 16; i++ {
		ch <- i
	}

	ok := a.DeliverStream("s1", 17)
	if ok {
		t.Fatalf("expected DeliverStream to fail when channel full")
	}

	select {
	case <-ch:
		// may read buffered items, not reliable
	default:
	}
	// It should close soon (CloseStream is invoked asynchronously).
	select {
	case _, more := <-ch:
		if more {
			// Channel isn't necessarily closed yet if we drained a value; keep waiting below.
		}
	case <-time.After(10 * time.Millisecond):
	}

	deadline := time.Now().Add(250 * time.Millisecond)
	for time.Now().Before(deadline) {
		select {
		case _, more := <-ch:
			if !more {
				return
			}
		default:
			time.Sleep(5 * time.Millisecond)
		}
	}
	t.Fatalf("expected stream channel to be closed after backpressure")
}
