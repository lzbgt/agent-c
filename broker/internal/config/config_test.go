package config

import "testing"

func TestBuildClientAuthRejectsEmptyClientID(t *testing.T) {
	_, err := BuildClientAuth([]ClientSpec{{ClientID: "", Token: "tok"}})
	if err == nil {
		t.Fatalf("expected error for empty client_id")
	}
}

func TestBuildClientAuthRejectsDuplicateToken(t *testing.T) {
	_, err := BuildClientAuth([]ClientSpec{{ClientID: "a", Token: "tok"}, {ClientID: "b", Token: "tok"}})
	if err == nil {
		t.Fatalf("expected error for duplicate token")
	}
}

func TestBuildClientAuthSuccess(t *testing.T) {
	ca, err := BuildClientAuth([]ClientSpec{{ClientID: "svc", Token: "tok", AllowedAgents: []string{"agent1"}}})
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if ca == nil || ca.ByToken == nil {
		t.Fatalf("client auth missing")
	}
	p := ca.ByToken["tok"]
	if p == nil {
		t.Fatalf("token not found")
	}
	if p.ClientID != "svc" {
		t.Fatalf("unexpected client_id: %q", p.ClientID)
	}
	if !p.AllowedAgents["agent1"] {
		t.Fatalf("allowed_agents missing")
	}
}
