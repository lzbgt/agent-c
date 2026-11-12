package workledger

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"sort"
	"strings"
)

type Requirement struct {
	Title     string   `json:"title"`
	Status    string   `json:"status"`
	DependsOn []string `json:"depends_on"`
	Evidence  string   `json:"evidence,omitempty"`
}
type Blocker struct {
	Status         string   `json:"status"`
	RequirementIDs []string `json:"requirement_ids"`
	Reason         string   `json:"reason"`
	RequiredAction string   `json:"required_action"`
	Attempts       []string `json:"attempts"`
	Evidence       string   `json:"evidence,omitempty"`
}
type State struct {
	Schema       string                 `json:"schema"`
	LedgerID     string                 `json:"ledger_id"`
	Revision     int64                  `json:"revision"`
	Requirements map[string]Requirement `json:"requirements"`
	Blockers     map[string]Blocker     `json:"blockers"`
}
type Snapshot struct {
	OK           bool     `json:"ok"`
	SessionID    string   `json:"session_id"`
	State        State    `json:"state"`
	Runnable     []string `json:"-"`
	Waiting      []string `json:"-"`
	OpenBlockers []string `json:"-"`
	AllDone      bool     `json:"-"`
	Digest       string   `json:"-"`
}

func (s *Snapshot) Validate(binding Binding) error {
	if !s.OK || s.SessionID != binding.SessionID || s.State.Schema != "agentd.work.v1" ||
		!ValidID(s.State.LedgerID) || s.State.Revision < 1 || s.State.Revision > 9007199254740991 {
		return fmt.Errorf("ledger is missing, uninitialized or has invalid authority identity/revision")
	}
	reqs := s.State.Requirements
	if len(reqs) == 0 || len(reqs) > 256 || s.State.Blockers == nil || len(s.State.Blockers) > 128 {
		return fmt.Errorf("ledger must contain 1..256 requirements and at most 128 blockers")
	}
	blocked := map[string]bool{}
	s.Runnable = nil
	s.Waiting = nil
	s.OpenBlockers = nil
	for id, b := range s.State.Blockers {
		if !ValidID(id) || (b.Status != "open" && b.Status != "resolved") || b.RequirementIDs == nil {
			return fmt.Errorf("invalid ledger blocker")
		}
		if strings.TrimSpace(b.Reason) == "" || len(b.Reason) > 4096 || strings.TrimSpace(b.RequiredAction) == "" || len(b.RequiredAction) > 4096 || len(b.Attempts) < 1 || len(b.Attempts) > 16 || len(b.RequirementIDs) > 256 {
			return fmt.Errorf("blocker lacks bounded recovery evidence")
		}
		for _, attempt := range b.Attempts {
			if strings.TrimSpace(attempt) == "" || len(attempt) > 4096 {
				return fmt.Errorf("invalid blocker recovery attempt")
			}
		}
		if b.Status == "resolved" && (strings.TrimSpace(b.Evidence) == "" || len(b.Evidence) > 4096) {
			return fmt.Errorf("resolved blocker lacks evidence")
		}
		for _, id := range b.RequirementIDs {
			if _, ok := reqs[id]; !ok {
				return fmt.Errorf("unknown blocked requirement")
			}
			if b.Status == "open" {
				blocked[id] = true
			}
		}
		if b.Status == "open" {
			s.OpenBlockers = append(s.OpenBlockers, id)
		}
	}
	visits := map[string]int{}
	var visit func(string) bool
	visit = func(id string) bool {
		r, ok := reqs[id]
		if !ok || visits[id] == 1 {
			return false
		}
		if visits[id] == 2 {
			return true
		}
		visits[id] = 1
		for _, dep := range r.DependsOn {
			if !visit(dep) {
				return false
			}
		}
		visits[id] = 2
		return true
	}
	s.AllDone = true
	for id, r := range reqs {
		if !ValidID(id) || strings.TrimSpace(r.Title) == "" || len(r.Title) > 256 || r.DependsOn == nil ||
			(r.Status != "pending" && r.Status != "running" && r.Status != "done") || !visit(id) {
			return fmt.Errorf("invalid ledger requirement or dependency graph")
		}
		ready := !blocked[id]
		for _, dep := range r.DependsOn {
			if reqs[dep].Status != "done" {
				ready = false
			}
		}
		if r.Status == "done" {
			if strings.TrimSpace(r.Evidence) == "" || len(r.Evidence) > 4096 || !ready {
				return fmt.Errorf("completed requirement lacks valid evidence or dependencies")
			}
		} else {
			s.AllDone = false
			if ready {
				s.Runnable = append(s.Runnable, id)
			} else {
				s.Waiting = append(s.Waiting, id)
			}
		}
	}
	sort.Strings(s.Runnable)
	sort.Strings(s.Waiting)
	sort.Strings(s.OpenBlockers)
	raw, err := json.Marshal(s.State)
	if err != nil {
		return err
	}
	sum := sha256.Sum256(raw)
	s.Digest = hex.EncodeToString(sum[:])
	return nil
}

// Summary is a bounded observation, never a second writable requirement ledger.
func (s Snapshot) Summary() map[string]any {
	rows := []map[string]any{}
	keys := make([]string, 0, len(s.State.Requirements))
	for id := range s.State.Requirements {
		keys = append(keys, id)
	}
	sort.Strings(keys)
	ready := map[string]bool{}
	for _, id := range s.Runnable {
		ready[id] = true
	}
	for _, id := range keys {
		r := s.State.Requirements[id]
		rows = append(rows, map[string]any{"id": id, "title": r.Title, "status": r.Status, "runnable": ready[id]})
	}
	return map[string]any{"ledger_id": s.State.LedgerID, "revision": s.State.Revision, "digest": s.Digest, "requirements": rows, "runnable": s.Runnable, "waiting": s.Waiting, "open_blockers": s.OpenBlockers, "all_requirements_done": s.AllDone}
}
