package config

import (
	"os"
	"path/filepath"
	"testing"
)

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

func TestLoadConnectorsFromFileArray(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "connectors.json")
	body := `[
  {"id":"slack","name":"Slack","kind":"chat","status":"ready","description":"Slack workspace"},
  {"id":"email","kind":"mail"}
]`
	if err := os.WriteFile(path, []byte(body), 0o600); err != nil {
		t.Fatalf("write: %v", err)
	}
	connectors, err := LoadConnectorsFromFile(path)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if len(connectors) != 2 {
		t.Fatalf("expected 2 connectors, got %d", len(connectors))
	}
	if connectors[0].ID != "slack" {
		t.Fatalf("unexpected id: %q", connectors[0].ID)
	}
}

func TestLoadConnectorsFromFileWrapper(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "connectors.json")
	body := `{"connectors":[{"id":"discord","kind":"chat","status":"disabled"}]}`
	if err := os.WriteFile(path, []byte(body), 0o600); err != nil {
		t.Fatalf("write: %v", err)
	}
	connectors, err := LoadConnectorsFromFile(path)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if len(connectors) != 1 {
		t.Fatalf("expected 1 connector, got %d", len(connectors))
	}
	if connectors[0].ID != "discord" {
		t.Fatalf("unexpected id: %q", connectors[0].ID)
	}
}
