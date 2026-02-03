package broker

import (
	"encoding/json"
	"testing"
)

func TestParseOrchestrateRequest_SafeDefaultsAndBigIntsPreserved(t *testing.T) {
	// 2^53+1, which would be lossy if parsed as float64.
	const big = "9007199254740993"

	body := []byte(`{
  "defaults": { "max_tokens": ` + big + ` },
  "tasks": [
    { "agent_id": "a-1", "request": { "prompt": "hi" } }
  ]
}`)

	parsed, err := parseOrchestrateRequest(body)
	if err != nil {
		t.Fatalf("parseOrchestrateRequest error: %v", err)
	}
	if len(parsed.Tasks) != 1 {
		t.Fatalf("expected 1 task, got %d", len(parsed.Tasks))
	}
	task := parsed.Tasks[0]
	if task.Path != "/api/v1/run" {
		t.Fatalf("expected default path /api/v1/run, got %q", task.Path)
	}
	if task.Method != "POST" {
		t.Fatalf("expected default method POST, got %q", task.Method)
	}

	var m map[string]json.RawMessage
	if err := json.Unmarshal(task.Body, &m); err != nil {
		t.Fatalf("task body unmarshal: %v", err)
	}
	if string(m["no_session"]) != "true" {
		t.Fatalf("expected no_session true, got %s", string(m["no_session"]))
	}
	if string(m["tools"]) != `"none"` {
		t.Fatalf("expected tools=none, got %s", string(m["tools"]))
	}
	if string(m["max_tokens"]) != big {
		t.Fatalf("expected max_tokens preserved as %s, got %s", big, string(m["max_tokens"]))
	}
}

func TestParseOrchestrateRequest_MissingPrompt(t *testing.T) {
	body := []byte(`{"tasks":[{"agent_id":"a-1","request":{"model":"x"}}]}`)
	if _, err := parseOrchestrateRequest(body); err == nil {
		t.Fatalf("expected error for missing prompt")
	}
}

func TestParseOrchestrateRequest_InvalidAgentID(t *testing.T) {
	body := []byte(`{"tasks":[{"agent_id":"no spaces","request":{"prompt":"x"}}]}`)
	if _, err := parseOrchestrateRequest(body); err == nil {
		t.Fatalf("expected error for invalid agent_id")
	}
}

