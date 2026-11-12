package workledger

import (
	"encoding/hex"
	"fmt"
)

// DispatchReceipt is broker-owned provenance, never caller-supplied evidence of
// host completion. An admitted entry can still have an unknown host outcome.
type DispatchReceipt struct {
	Schema            string  `json:"schema"`
	TeamID            string  `json:"team_id"`
	OrchestratorRunID string  `json:"orchestrator_run_id"`
	DispatchID        string  `json:"dispatch_id"`
	Binding           Binding `json:"binding"`
	LedgerID          string  `json:"ledger_id"`
	Revision          int64   `json:"revision"`
	Digest            string  `json:"digest"`
	ControlDigest     string  `json:"control_digest"`
	State             string  `json:"state"`
	TeamRunID         string  `json:"team_run_id,omitempty"`
	AdmittedUnixMS    int64   `json:"admitted_unix_ms"`
	RecordedUnixMS    int64   `json:"recorded_unix_ms,omitempty"`
}

func ValidDigest(value string) bool {
	if len(value) != 64 {
		return false
	}
	raw, err := hex.DecodeString(value)
	return err == nil && hex.EncodeToString(raw) == value
}
func (r DispatchReceipt) ValidateIdentity() error {
	if r.Schema != "agentd.goal-dispatch.v1" || !ValidID(r.TeamID) || !ValidID(r.OrchestratorRunID) ||
		!ValidID(r.DispatchID) || !ValidID(r.LedgerID) || r.Revision < 1 || r.Revision > 9007199254740991 ||
		!ValidDigest(r.Digest) || !ValidDigest(r.ControlDigest) {
		return fmt.Errorf("invalid dispatch receipt identity")
	}
	if _, err := ParseBinding(r.Binding.Map()); err != nil {
		return err
	}
	return nil
}
func (r DispatchReceipt) Validate() error {
	if err := r.ValidateIdentity(); err != nil {
		return err
	}
	if r.AdmittedUnixMS <= 0 || r.AdmittedUnixMS > 9007199254740991 {
		return fmt.Errorf("invalid admission timestamp")
	}
	switch r.State {
	case "admitted":
		if r.TeamRunID != "" || r.RecordedUnixMS != 0 {
			return fmt.Errorf("pending receipt contains a result")
		}
	case "recorded":
		if !ValidID(r.TeamRunID) || r.RecordedUnixMS < r.AdmittedUnixMS || r.RecordedUnixMS > 9007199254740991 {
			return fmt.Errorf("invalid recorded receipt")
		}
	default:
		return fmt.Errorf("invalid dispatch receipt state")
	}
	return nil
}
