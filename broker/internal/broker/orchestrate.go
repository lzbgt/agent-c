package broker

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"strings"
	"sync/atomic"
	"time"

	"agentd-broker/internal/db"
)

type orchestrateTaskPrepared struct {
	TaskID  string
	AgentID string
	DeploymentID string
	Method  string
	Path    string
	Query   string

	Headers map[string]string
	Body    []byte
}

type orchestrateParsed struct {
	MaxConcurrency int
	TimeoutMS      int
	AllowSessions  bool
	TraceID        string
	Tasks          []orchestrateTaskPrepared
}

func parseOrchestrateRequest(body []byte, defaultTraceID string) (orchestrateParsed, error) {
	var root map[string]json.RawMessage
	if err := json.Unmarshal(body, &root); err != nil {
		return orchestrateParsed{}, errors.New("invalid json")
	}

	out := orchestrateParsed{
		MaxConcurrency: 4,
		TimeoutMS:      60_000,
		AllowSessions:  false,
	}

	traceID := strings.TrimSpace(defaultTraceID)
	if v, ok := root["trace_id"]; ok && len(v) > 0 && string(v) != "null" {
		var s string
		if err := json.Unmarshal(v, &s); err == nil && strings.TrimSpace(s) != "" {
			traceID = strings.TrimSpace(s)
		}
	}
	if traceID != "" && !isSafeTraceID(traceID) {
		// Fall back to the broker-generated trace id instead of dropping correlation entirely.
		traceID = strings.TrimSpace(defaultTraceID)
		if traceID != "" && !isSafeTraceID(traceID) {
			traceID = ""
		}
	}
	out.TraceID = traceID

	if v, ok := root["max_concurrency"]; ok {
		var mc int
		if err := json.Unmarshal(v, &mc); err == nil {
			if mc < 1 {
				mc = 1
			}
			if mc > 16 {
				mc = 16
			}
			out.MaxConcurrency = mc
		}
	}
	if v, ok := root["timeout_ms"]; ok {
		var tm int
		if err := json.Unmarshal(v, &tm); err == nil {
			if tm < 100 {
				tm = 100
			}
			if tm > 300_000 {
				tm = 300_000
			}
			out.TimeoutMS = tm
		}
	}
	if v, ok := root["allow_sessions"]; ok {
		var as bool
		if err := json.Unmarshal(v, &as); err == nil {
			out.AllowSessions = as
		}
	}

	var defaults map[string]json.RawMessage
	if v, ok := root["defaults"]; ok && len(v) > 0 && string(v) != "null" {
		_ = json.Unmarshal(v, &defaults)
	}

	var tasksIn []map[string]json.RawMessage
	if v, ok := root["tasks"]; ok {
		if err := json.Unmarshal(v, &tasksIn); err != nil {
			return orchestrateParsed{}, errors.New("invalid tasks")
		}
	}
	if len(tasksIn) == 0 {
		return orchestrateParsed{}, errors.New("missing tasks")
	}
	if len(tasksIn) > 32 {
		return orchestrateParsed{}, errors.New("too many tasks")
	}

	out.Tasks = make([]orchestrateTaskPrepared, 0, len(tasksIn))
	for i, t := range tasksIn {
		var agentID string
		if v, ok := t["agent_id"]; ok {
			_ = json.Unmarshal(v, &agentID)
		}
		agentID = strings.TrimSpace(agentID)
		if agentID == "" {
			return orchestrateParsed{}, errors.New("task missing agent_id")
		}
		if !agentIDRe.MatchString(agentID) {
			return orchestrateParsed{}, errors.New("invalid agent_id")
		}
		var deploymentID string
		if v, ok := t["deployment_id"]; ok {
			_ = json.Unmarshal(v, &deploymentID)
		}
		if deploymentID == "" {
			if v, ok := t["deployment"]; ok {
				_ = json.Unmarshal(v, &deploymentID)
			}
		}
		deploymentID = strings.TrimSpace(deploymentID)
		if deploymentID != "" && !deploymentIDRe.MatchString(deploymentID) {
			return orchestrateParsed{}, errors.New("invalid deployment_id")
		}

		taskID := "task_" + itoa(i)
		if v, ok := t["task_id"]; ok {
			var s string
			if err := json.Unmarshal(v, &s); err == nil && strings.TrimSpace(s) != "" {
				taskID = s
			}
		}

		method := "POST"
		if v, ok := t["method"]; ok {
			var s string
			if err := json.Unmarshal(v, &s); err == nil && strings.TrimSpace(s) != "" {
				method = strings.ToUpper(strings.TrimSpace(s))
			}
		}
		path := "/api/v1/run"
		if v, ok := t["path"]; ok {
			var s string
			if err := json.Unmarshal(v, &s); err == nil && strings.TrimSpace(s) != "" {
				path = strings.TrimSpace(s)
			}
		}
		query := ""
		if v, ok := t["query"]; ok {
			_ = json.Unmarshal(v, &query)
		}

		if !strings.HasPrefix(path, "/api/") {
			return orchestrateParsed{}, errors.New("task path must start with /api/")
		}
		if strings.Contains(path, "..") {
			return orchestrateParsed{}, errors.New("task path contains ..")
		}

		var hdrs map[string]string
		if v, ok := t["headers"]; ok && len(v) > 0 && string(v) != "null" {
			_ = json.Unmarshal(v, &hdrs)
		}
		if hdrs == nil {
			hdrs = map[string]string{}
		}

		// Determine request object: either task.request or fallback to task itself (minus meta keys).
		runReq := map[string]json.RawMessage{}
		if v, ok := t["request"]; ok && len(v) > 0 && string(v) != "null" {
			if err := json.Unmarshal(v, &runReq); err != nil {
				return orchestrateParsed{}, errors.New("invalid task.request")
			}
		} else {
			for k, v := range t {
				switch k {
				case "agent_id", "deployment_id", "deployment", "task_id", "method", "path", "query", "headers", "request":
					continue
				default:
					runReq[k] = v
				}
			}
		}

		// Apply defaults for missing keys only.
		for k, v := range defaults {
			if _, ok := runReq[k]; ok {
				continue
			}
			runReq[k] = v
		}

		if !out.AllowSessions {
			runReq["no_session"] = json.RawMessage("true")
			if _, ok := runReq["tools"]; !ok {
				runReq["tools"] = json.RawMessage(`"none"`)
			}
		}
		if out.TraceID != "" {
			if _, ok := runReq["trace_id"]; !ok {
				if tb, err := json.Marshal(out.TraceID); err == nil && len(tb) > 0 {
					runReq["trace_id"] = json.RawMessage(tb)
				}
			}
		}

		// Require a prompt so this remains a "run orchestrator" endpoint by default.
		{
			v, ok := runReq["prompt"]
			if !ok {
				return orchestrateParsed{}, errors.New("task missing prompt")
			}
			var s string
			if err := json.Unmarshal(v, &s); err != nil || strings.TrimSpace(s) == "" {
				return orchestrateParsed{}, errors.New("task missing prompt")
			}
		}

		bodyBytes, err := json.Marshal(runReq)
		if err != nil {
			return orchestrateParsed{}, errors.New("failed to build task body")
		}

		out.Tasks = append(out.Tasks, orchestrateTaskPrepared{
			TaskID:  taskID,
			AgentID: agentID,
			DeploymentID: deploymentID,
			Method:  method,
			Path:    path,
			Query:   query,
			Headers: hdrs,
			Body:    bodyBytes,
		})
	}

	return out, nil
}

func itoa(i int) string {
	// Small internal helper to avoid fmt import in hot code paths.
	if i == 0 {
		return "0"
	}
	neg := i < 0
	if neg {
		i = -i
	}
	var buf [32]byte
	n := len(buf)
	for i > 0 {
		n--
		buf[n] = byte('0' + (i % 10))
		i /= 10
	}
	if neg {
		n--
		buf[n] = '-'
	}
	return string(buf[n:])
}

func (s *Server) handleOrchestrate(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	p, err := s.requirePrincipal(r)
	if err != nil {
		http.Error(w, err.Error(), http.StatusUnauthorized)
		return
	}

	body, err := readBodyBounded(r.Body, s.cfg.MaxRequestBodySize)
	if err != nil {
		http.Error(w, "request body too large", http.StatusRequestEntityTooLarge)
		return
	}
	parsed, err := parseOrchestrateRequest(body, traceIDFromContext(r.Context()))
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}

	// Safety: ensure the user can access every referenced agent before fanning out.
	for _, t := range parsed.Tasks {
		if ok, err := s.canAccessAgent(r.Context(), p, t.AgentID); err != nil {
			http.Error(w, "db error", http.StatusInternalServerError)
			return
		} else if !ok {
			http.Error(w, "forbidden", http.StatusForbidden)
			return
		}
	}

	idemKey, idemErr := idempotencyKeyFromRequest(r)
	if idemErr != nil {
		http.Error(w, idemErr.Error(), http.StatusBadRequest)
		return
	}
	var idemStatus db.IdempotencyStatus
	if idemKey != "" {
		reqHash := idempotencyRequestHash(r.Method, r.URL.Path, r.URL.RawQuery, "", "", body)
		claim, err := s.cfg.DB.ClaimIdempotency(r.Context(), db.IdempotencyRecord{
			UserSub:       p.Sub,
			Key:           idemKey,
			RequestSHA256: reqHash,
			Method:        r.Method,
			Path:          r.URL.Path,
			Query:         r.URL.RawQuery,
			AgentID:       "",
			ExpiresAt:     time.Now().Add(s.cfg.IdempotencyTTL),
		})
		if err != nil {
			http.Error(w, "idempotency claim failed", http.StatusInternalServerError)
			return
		}
		idemStatus = claim.Status
		switch claim.Status {
		case db.IdempotencyReplay:
			w.Header().Set("X-Idempotency-Key", idemKey)
			w.Header().Set("X-Idempotency-Replay", "true")
			if claim.Record != nil {
				for k, v := range claim.Record.ResponseHeaders {
					w.Header().Set(k, v)
				}
				if claim.Record.ResponseStatus > 0 {
					w.WriteHeader(claim.Record.ResponseStatus)
				}
				if len(claim.Record.ResponseBody) > 0 {
					_, _ = w.Write(claim.Record.ResponseBody)
				}
				return
			}
			w.WriteHeader(http.StatusOK)
			_, _ = w.Write([]byte(`{"ok":true,"replayed":true}`))
			return
		case db.IdempotencyInProgress:
			w.Header().Set("X-Idempotency-Key", idemKey)
			w.Header().Set("Retry-After", "1")
			w.WriteHeader(http.StatusConflict)
			writeJSON(w, map[string]any{
				"ok":    false,
				"error": "idempotency_key_in_progress",
			})
			return
		case db.IdempotencyConflict:
			w.Header().Set("X-Idempotency-Key", idemKey)
			w.WriteHeader(http.StatusConflict)
			writeJSON(w, map[string]any{
				"ok":    false,
				"error": "idempotency_key_conflict",
			})
			return
		case db.IdempotencyCreated:
			// continue
		}
	}

	// Execute tasks concurrently (bounded).
	type taskOut struct {
		TaskID     string         `json:"task_id"`
		AgentID    string         `json:"agent_id"`
		DeploymentID string         `json:"deployment_id,omitempty"`
		OK         bool           `json:"ok"`
		MS         int            `json:"ms"`
		HTTPStatus int            `json:"http_status,omitempty"`
		Error      string         `json:"error,omitempty"`
		Result     map[string]any `json:"result,omitempty"`
	}

	out := make([]taskOut, len(parsed.Tasks))
	var next atomic.Uint32

	worker := func() {
		for {
			idx := int(next.Add(1) - 1)
			if idx >= len(parsed.Tasks) {
				return
			}
			t := parsed.Tasks[idx]

			// Build forwarded headers.
			fwdHeaders := map[string]string{}
			for k, v := range t.Headers {
				if strings.TrimSpace(k) == "" {
					continue
				}
				kl := strings.ToLower(strings.TrimSpace(k))
				if kl == "authorization" || kl == "host" || kl == "connection" {
					continue
				}
				fwdHeaders[k] = v
			}
			fwdHeaders["X-Agentd-Broker-User"] = p.Sub
			if parsed.TraceID != "" {
				if _, ok := fwdHeaders["X-Trace-ID"]; !ok {
					fwdHeaders["X-Trace-ID"] = parsed.TraceID
				}
			}

			ctx := r.Context()
			timeout := time.Duration(parsed.TimeoutMS) * time.Millisecond
			var cancel context.CancelFunc
			if timeout > 0 {
				ctx, cancel = context.WithTimeout(ctx, timeout)
			}

			start := time.Now()
			ro := s.relayAgentHTTP(ctx, p, t.AgentID, t.DeploymentID, t.Method, t.Path, t.Query, fwdHeaders, t.Body)
			ms := int(time.Since(start).Milliseconds())
			if cancel != nil {
				cancel()
			}

			row := taskOut{
				TaskID:       t.TaskID,
				AgentID:      t.AgentID,
				DeploymentID: t.DeploymentID,
				MS:           ms,
			}
			if ro.BrokerStatus != 0 {
				row.OK = false
				row.Error = ro.Err
				out[idx] = row
				continue
			}
			row.HTTPStatus = ro.AgentStatus

			var resultObj map[string]any
			if err := json.Unmarshal(ro.Body, &resultObj); err == nil {
				row.Result = resultObj
				if v, ok := resultObj["ok"].(bool); ok {
					row.OK = v
				} else {
					row.OK = ro.AgentStatus >= 200 && ro.AgentStatus < 300
				}
			} else {
				row.OK = ro.AgentStatus >= 200 && ro.AgentStatus < 300
				row.Error = "invalid json response"
			}
			out[idx] = row
		}
	}

	threads := parsed.MaxConcurrency
	if threads > len(parsed.Tasks) {
		threads = len(parsed.Tasks)
	}
	if threads < 1 {
		threads = 1
	}

	done := make(chan struct{}, threads)
	for i := 0; i < threads; i++ {
		go func() {
			worker()
			done <- struct{}{}
		}()
	}
	for i := 0; i < threads; i++ {
		<-done
	}

	allOK := true
	for _, r := range out {
		if !r.OK {
			allOK = false
			break
		}
	}

	respObj := map[string]any{
		"ok":              true,
		"trace_id":        parsed.TraceID,
		"all_ok":          allOK,
		"tasks_total":     len(out),
		"max_concurrency": threads,
		"timeout_ms":      parsed.TimeoutMS,
		"results":         out,
	}
	respBody := mustJSON(respObj)

	// Persist an orchestrate audit row keyed by trace_id so UIs can correlate runs even when
	// the underlying agentd requests used no_session=true (no agent-side audit persistence).
	if s.cfg.DB != nil && parsed.TraceID != "" {
		safeReq := scrubJSONForAudit(body)
		safeResp := scrubJSONForAudit(mustJSON(respObj))
		_ = s.cfg.DB.InsertOrchestrateAudit(r.Context(), p.Sub, parsed.TraceID, safeReq, safeResp)
	}

	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	if idemKey != "" {
		w.Header().Set("X-Idempotency-Key", idemKey)
		if idemStatus == db.IdempotencyCreated {
			w.Header().Set("X-Idempotency-Replay", "false")
		}
	}
	if idemKey != "" {
		if int64(len(respBody)) <= s.cfg.IdempotencyMaxBodyBytes {
			respHeaders := map[string]string{
				"Content-Type": "application/json; charset=utf-8",
				"X-Request-ID": requestIDFromContext(r.Context()),
				"X-Trace-ID":   traceIDFromContext(r.Context()),
			}
			_ = s.cfg.DB.CompleteIdempotency(r.Context(), p.Sub, idemKey, http.StatusOK, respHeaders, respBody)
		} else {
			_ = s.cfg.DB.DeleteIdempotency(r.Context(), p.Sub, idemKey)
			w.Header().Set("X-Idempotency-Disabled", "response_too_large")
		}
	}
	_, _ = w.Write(respBody)
}

func mustJSON(v any) []byte {
	b, _ := json.Marshal(v)
	if len(b) == 0 {
		return []byte("{}")
	}
	return b
}

func scrubJSONForAudit(raw []byte) []byte {
	// Best-effort: redact secrets and drop large fields from stored audit blobs.
	// If parsing fails, store an empty object.
	var v any
	if err := json.Unmarshal(raw, &v); err != nil {
		return []byte("{}")
	}
	scrubValue(&v)
	out, err := json.Marshal(v)
	if err != nil || len(out) == 0 {
		return []byte("{}")
	}
	// Hard cap to keep broker DB from being used as a blob store.
	const max = 256 * 1024
	if len(out) > max {
		// Replace with a small sentinel.
		return []byte(`{"ok":true,"truncated":true}`)
	}
	return out
}

func scrubValue(v *any) {
	if v == nil || *v == nil {
		return
	}
	switch x := (*v).(type) {
	case map[string]any:
		for k, vv := range x {
			kl := strings.ToLower(strings.TrimSpace(k))
			// Obvious secrets.
			if kl == "api_key" || kl == "authorization" || kl == "x-agentd-authorization" || kl == "token" || kl == "password" || kl == "secret" {
				x[k] = "[redacted]"
				continue
			}
			// Large-ish fields we don't want in audit.
			if kl == "trace_text" || kl == "http_body" || kl == "events" {
				delete(x, k)
				continue
			}
			child := vv
			scrubValue(&child)
			x[k] = child
		}
	case []any:
		for i := range x {
			child := x[i]
			scrubValue(&child)
			x[i] = child
		}
	}
}
