package workledger

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
)

// ControlDigest excludes only coordinator observations. A goal, owner, template,
// binding or role-plan edit invalidates an admitted-but-not-submitted turn.
func ControlDigest(goal string, contract, roles, meta map[string]any) string {
	if contract == nil {
		contract = map[string]any{}
	}
	if roles == nil {
		roles = map[string]any{}
	}
	control := map[string]any{}
	for k, v := range meta {
		if k != "work_ledger_runtime" {
			control[k] = v
		}
	}
	raw, err := json.Marshal(map[string]any{"goal": goal, "goal_contract": contract, "role_plan_snapshot": roles, "meta": control})
	if err != nil {
		return ""
	}
	sum := sha256.Sum256(raw)
	return hex.EncodeToString(sum[:])
}
