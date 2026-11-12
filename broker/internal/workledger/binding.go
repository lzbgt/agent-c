// Package workledger defines the read-only contract shared by the broker and coordinator.
package workledger

import (
	"encoding/json"
	"fmt"
	"regexp"
)

type Binding struct {
	AgentID      string `json:"agent_id"`
	DeploymentID string `json:"deployment_id"`
	SessionID    string `json:"session_id"`
}

var safeID = regexp.MustCompile(`^[A-Za-z0-9_.-]{1,200}$`)

func ValidID(s string) bool { return safeID.MatchString(s) && !containsTraversal(s) }
func containsTraversal(s string) bool {
	for i := 0; i+1 < len(s); i++ {
		if s[i:i+2] == ".." {
			return true
		}
	}
	return s == "."
}
func ParseBinding(v any) (Binding, error) {
	var b Binding
	m, ok := v.(map[string]any)
	if !ok || len(m) != 3 {
		return b, fmt.Errorf("work_ledger requires agent_id, deployment_id and session_id only")
	}
	raw, err := json.Marshal(m)
	if err != nil {
		return b, err
	}
	if err = json.Unmarshal(raw, &b); err != nil || !ValidID(b.AgentID) || !ValidID(b.DeploymentID) || !ValidID(b.SessionID) {
		return b, fmt.Errorf("work_ledger requires safe explicit authority identifiers")
	}
	return b, nil
}
func (b Binding) Map() map[string]any {
	return map[string]any{"agent_id": b.AgentID, "deployment_id": b.DeploymentID, "session_id": b.SessionID}
}
