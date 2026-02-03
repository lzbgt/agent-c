package broker

import (
	"context"
	"encoding/json"
	"net/http"
	"sort"
	"strings"
	"time"
)

type traceRelayAuditRow struct {
	TSUnixMS  int64  `json:"ts_unix_ms"`
	AgentID   string `json:"agent_id"`
	Method    string `json:"method"`
	Path      string `json:"path"`
	Status    int    `json:"status"`
	LatencyMS int    `json:"latency_ms"`
	Error     string `json:"error,omitempty"`
}

type traceAgentdResult struct {
	AgentID     string         `json:"agent_id"`
	OK          bool           `json:"ok"`
	MS          int            `json:"ms"`
	HTTPStatus  int            `json:"http_status,omitempty"`
	AgentStatus int            `json:"agent_status,omitempty"`
	Error       string         `json:"error,omitempty"`
	Body        map[string]any `json:"body,omitempty"`
}

func (s *Server) handleTrace(w http.ResponseWriter, r *http.Request) {
	if r.Method != "GET" {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	p, err := s.requirePrincipal(r)
	if err != nil {
		http.Error(w, err.Error(), http.StatusUnauthorized)
		return
	}
	traceID := strings.TrimSpace(r.URL.Query().Get("trace_id"))
	if traceID == "" {
		http.Error(w, "missing trace_id", http.StatusBadRequest)
		return
	}
	if !isSafeTraceID(traceID) {
		http.Error(w, "invalid trace_id", http.StatusBadRequest)
		return
	}

	limit := 100
	if v := strings.TrimSpace(r.URL.Query().Get("limit")); v != "" {
		// small and safe; ignore parse errors
		if n, ok := parseIntBounded(v, 1, 500); ok {
			limit = n
		}
	}

	rows, err := s.cfg.DB.ListRelayAuditByTrace(r.Context(), p.Sub, traceID, limit)
	if err != nil {
		http.Error(w, "db error", http.StatusInternalServerError)
		return
	}

	relay := make([]traceRelayAuditRow, 0, len(rows))
	agentSet := map[string]bool{}
	for _, rr := range rows {
		relay = append(relay, traceRelayAuditRow{
			TSUnixMS:  rr.TSUnixMS,
			AgentID:   rr.AgentID,
			Method:    rr.Method,
			Path:      rr.Path,
			Status:    rr.Status,
			LatencyMS: rr.LatencyMS,
			Error:     rr.Error,
		})
		if strings.TrimSpace(rr.AgentID) != "" {
			agentSet[rr.AgentID] = true
		}
	}

	// Fan-out to referenced agents to pull their agentd audit records for this trace_id.
	// This is best-effort: failures are returned per-agent without failing the overall response.
	fanout := true
	if v := strings.TrimSpace(r.URL.Query().Get("fanout")); v != "" {
		fanout = v == "1" || strings.EqualFold(v, "true")
	}
	agentd := []traceAgentdResult{}
	if fanout && len(agentSet) > 0 {
		agentIDs := make([]string, 0, len(agentSet))
		for id := range agentSet {
			agentIDs = append(agentIDs, id)
		}
		sort.Strings(agentIDs)
		if len(agentIDs) > 16 {
			agentIDs = agentIDs[:16]
		}

		fwdHeaders := map[string]string{
			"X-Agentd-Broker-User": p.Sub,
			"X-Trace-ID":           traceID,
		}
		// Optional pass-through for agentd bearer token.
		if v := strings.TrimSpace(r.Header.Get("X-Agentd-Authorization")); v != "" {
			fwdHeaders["Authorization"] = v
		}

		for _, agentID := range agentIDs {
			ctx := r.Context()
			ctx, cancel := context.WithTimeout(ctx, 12*time.Second)
			start := time.Now()
			path := "/api/v1/trace"
			query := "trace_id=" + urlQueryEscape(traceID) + "&limit=200&max_bytes=524288"
			ro := s.relayAgentHTTP(ctx, p, agentID, "GET", path, query, fwdHeaders, nil)
			ms := int(time.Since(start).Milliseconds())
			cancel()

			res := traceAgentdResult{
				AgentID: agentID,
				MS:      ms,
			}
			if ro.BrokerStatus != 0 {
				res.OK = false
				res.Error = ro.Err
				agentd = append(agentd, res)
				continue
			}
			res.AgentStatus = ro.AgentStatus
			res.HTTPStatus = ro.AgentStatus

			var body map[string]any
			if err := json.Unmarshal(ro.Body, &body); err != nil {
				res.OK = false
				res.Error = "invalid json response"
				agentd = append(agentd, res)
				continue
			}
			res.Body = body
			if v, ok := body["ok"].(bool); ok {
				res.OK = v
			} else {
				res.OK = ro.AgentStatus >= 200 && ro.AgentStatus < 300
			}
			agentd = append(agentd, res)
		}
	}

	writeJSON(w, map[string]any{
		"ok":         true,
		"trace_id":   traceID,
		"relay_audit": relay,
		"agentd":     agentd,
	})
}

// parseIntBounded parses a base-10 int and clamps it.
func parseIntBounded(raw string, lo, hi int) (int, bool) {
	raw = strings.TrimSpace(raw)
	if raw == "" {
		return 0, false
	}
	n := 0
	for _, r := range raw {
		if r < '0' || r > '9' {
			return 0, false
		}
		n = n*10 + int(r-'0')
		if n > hi {
			n = hi
			break
		}
	}
	if n < lo {
		n = lo
	}
	return n, true
}

func urlQueryEscape(s string) string {
	// Avoid importing net/url in hot paths; this is only used for trace id fanout.
	// trace_id is already validated to a safe character set, so no escaping is required.
	return s
}

