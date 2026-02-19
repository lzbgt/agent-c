package broker

import (
	"context"
	"encoding/base64"
	"net/http"
	"strings"
	"time"

	"agentd-broker/internal/db"
	"agentd-broker/internal/events"
	"agentd-broker/internal/proto"
)

type relayOutcome struct {
	BrokerStatus int
	AgentStatus  int
	Headers      map[string]string
	Body         []byte
	Err          string
	LatencyMS    int
}

func (s *Server) auditRelay(ctx context.Context, p *Principal, agentID, deploymentID, method, agentPath string, status, latencyMS int, errStr string) {
	if p == nil {
		return
	}
	tid := traceIDFromContext(ctx)
	_ = s.cfg.DB.InsertRelayAudit(ctx, p.Sub, agentID, method, agentPath, status, latencyMS, errStr, tid)
	s.cfg.Events.PublishTo([]string{p.Sub}, events.Event{
		Type:    "relay_audit",
		AgentID: agentID,
		UserSub: p.Sub,
		TraceID: tid,
		Payload: map[string]any{
			"method":        method,
			"path":          agentPath,
			"deployment_id": deploymentID,
			"status":        status,
			"latency_ms":    latencyMS,
			"error":         errStr,
		},
	})
}

func (s *Server) relayAgentHTTP(
	ctx context.Context,
	p *Principal,
	agentID string,
	deploymentID string,
	method string,
	agentPath string,
	rawQuery string,
	headers map[string]string,
	body []byte,
) relayOutcome {
	start := time.Now()
	out := relayOutcome{
		Headers: map[string]string{},
	}

	a, err := s.cfg.Registry.Require(agentID, deploymentID)
	if err != nil {
		out.BrokerStatus = http.StatusBadGateway
		out.Err = "agent not connected"
		out.LatencyMS = int(time.Since(start).Milliseconds())
		s.auditRelay(ctx, p, agentID, deploymentID, method, agentPath, 0, out.LatencyMS, out.Err)
		return out
	}

	reqID := newID()
	ch, err := a.RegisterPending(reqID)
	if err != nil {
		out.BrokerStatus = http.StatusServiceUnavailable
		out.Err = "broker overloaded"
		out.LatencyMS = int(time.Since(start).Milliseconds())
		s.auditRelay(ctx, p, agentID, deploymentID, method, agentPath, 0, out.LatencyMS, out.Err)
		return out
	}
	defer a.UnregisterPending(reqID)

	msg := proto.RelayRequest{
		Type: proto.TypeHTTPRequest,
		ID:   reqID,
		Req: proto.HTTPRequest{
			Method:  method,
			Path:    agentPath,
			Query:   rawQuery,
			Headers: headers,
			BodyB64: base64.StdEncoding.EncodeToString(body),
		},
	}

	if err := a.Send(msg); err != nil {
		s.cfg.Registry.Delete(agentID, deploymentID)
		out.BrokerStatus = http.StatusBadGateway
		out.Err = "agent send failed"
		out.LatencyMS = int(time.Since(start).Milliseconds())
		s.auditRelay(ctx, p, agentID, deploymentID, method, agentPath, 0, out.LatencyMS, out.Err)
		return out
	}

	timeout := 60 * time.Second
	if dl, ok := ctx.Deadline(); ok {
		timeout = time.Until(dl)
	}
	if timeout < 100*time.Millisecond {
		timeout = 100 * time.Millisecond
	}
	timer := time.NewTimer(timeout)
	defer timer.Stop()

	select {
	case resp, ok := <-ch:
		if !ok {
			out.BrokerStatus = http.StatusBadGateway
			out.Err = "agent disconnected"
			out.LatencyMS = int(time.Since(start).Milliseconds())
			s.auditRelay(ctx, p, agentID, deploymentID, method, agentPath, 0, out.LatencyMS, out.Err)
			return out
		}
		if resp.Err != "" {
			out.BrokerStatus = http.StatusBadGateway
			out.Err = resp.Err
			out.LatencyMS = int(time.Since(start).Milliseconds())
			s.auditRelay(ctx, p, agentID, deploymentID, method, agentPath, 0, out.LatencyMS, out.Err)
			return out
		}

		out.AgentStatus = resp.Resp.Status
		for k, v := range resp.Resp.Headers {
			kl := strings.ToLower(k)
			if kl == "content-length" || kl == "connection" || kl == "transfer-encoding" {
				continue
			}
			out.Headers[k] = v
		}
		b, err := base64.StdEncoding.DecodeString(resp.Resp.BodyB64)
		if err != nil {
			out.BrokerStatus = http.StatusBadGateway
			out.Err = "invalid agent body"
			out.LatencyMS = int(time.Since(start).Milliseconds())
			s.auditRelay(ctx, p, agentID, deploymentID, method, agentPath, out.AgentStatus, out.LatencyMS, out.Err)
			return out
		}
		out.Body = b
		out.LatencyMS = int(time.Since(start).Milliseconds())
		s.auditRelay(ctx, p, agentID, deploymentID, method, agentPath, out.AgentStatus, out.LatencyMS, "")
		return out

	case <-timer.C:
		out.BrokerStatus = http.StatusGatewayTimeout
		out.Err = "agent timeout"
		out.LatencyMS = int(time.Since(start).Milliseconds())
		s.auditRelay(ctx, p, agentID, deploymentID, method, agentPath, 0, out.LatencyMS, out.Err)
		return out
	}
}

func buildAgentForwardHeaders(r *http.Request, userSub string) map[string]string {
	headers := map[string]string{}
	if r == nil {
		return headers
	}
	for k, vv := range r.Header {
		if len(vv) == 0 {
			continue
		}
		kl := strings.ToLower(k)
		if kl == "authorization" || kl == "host" || kl == "connection" || kl == "idempotency-key" || kl == "x-idempotency-key" || kl == "x-agentd-deployment" {
			continue
		}
		headers[k] = vv[0]
	}
	hasHeader := func(name string) bool {
		for k := range headers {
			if strings.EqualFold(k, name) {
				return true
			}
		}
		return false
	}
	if !hasHeader("X-Request-ID") {
		if rid := requestIDFromContext(r.Context()); rid != "" {
			headers["X-Request-ID"] = rid
		}
	}
	if !hasHeader("X-Trace-ID") {
		if tid := traceIDFromContext(r.Context()); tid != "" {
			headers["X-Trace-ID"] = tid
		}
	}
	if userSub != "" {
		headers["X-Agentd-Broker-User"] = userSub
	}
	if v := strings.TrimSpace(r.Header.Get("X-Agentd-Authorization")); v != "" {
		headers["Authorization"] = v
		delete(headers, "X-Agentd-Authorization")
	}
	return headers
}

func (s *Server) handleAgentProxy(w http.ResponseWriter, r *http.Request, agentID, agentPath string) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if ok, err := s.canAccessAgent(r.Context(), p, agentID); err != nil {
		writeErrorJSON(w, "db error", http.StatusInternalServerError)
		return
	} else if !ok {
		writeErrorJSON(w, "forbidden", http.StatusForbidden)
		return
	}
	deploymentID, derr := deploymentIDFromRequest(r)
	if derr != nil {
		writeErrorJSON(w, derr.Error(), http.StatusBadRequest)
		return
	}

	// Read request body (bounded).
	body, err := readBodyBounded(r.Body, s.cfg.MaxRequestBodySize)
	if err != nil {
		writeErrorJSON(w, "request body too large", http.StatusRequestEntityTooLarge)
		return
	}
	idemKey, idemErr := idempotencyKeyFromRequest(r)
	if idemErr != nil {
		writeErrorJSON(w, idemErr.Error(), http.StatusBadRequest)
		return
	}
	var idemStatus db.IdempotencyStatus
	if idemKey != "" {
		reqHash := idempotencyRequestHash(r.Method, agentPath, r.URL.RawQuery, agentID, deploymentID, body)
		claim, err := s.cfg.DB.ClaimIdempotency(r.Context(), db.IdempotencyRecord{
			UserSub:       p.Sub,
			Key:           idemKey,
			RequestSHA256: reqHash,
			Method:        r.Method,
			Path:          agentPath,
			Query:         r.URL.RawQuery,
			AgentID:       agentID,
			ExpiresAt:     time.Now().Add(s.cfg.IdempotencyTTL),
		})
		if err != nil {
			writeErrorJSON(w, "idempotency claim failed", http.StatusInternalServerError)
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
			writeJSON(w, errorEnvelope("idempotency_key_in_progress"))
			return
		case db.IdempotencyConflict:
			w.Header().Set("X-Idempotency-Key", idemKey)
			w.WriteHeader(http.StatusConflict)
			writeJSON(w, errorEnvelope("idempotency_key_conflict"))
			return
		case db.IdempotencyCreated:
			// continue
		}
	}

	// Build agent-facing request.
	headers := buildAgentForwardHeaders(r, p.Sub)

	ro := s.relayAgentHTTP(r.Context(), p, agentID, deploymentID, r.Method, agentPath, r.URL.RawQuery, headers, body)
	if ro.BrokerStatus != 0 {
		if idemKey != "" {
			_ = s.cfg.DB.DeleteIdempotency(r.Context(), p.Sub, idemKey)
		}
		writeErrorJSON(w, ro.Err, ro.BrokerStatus)
		return
	}
	for k, v := range ro.Headers {
		w.Header().Set(k, v)
	}
	if idemKey != "" {
		w.Header().Set("X-Idempotency-Key", idemKey)
		if idemStatus == db.IdempotencyCreated {
			w.Header().Set("X-Idempotency-Replay", "false")
		}
	}
	storeIdem := idemKey != "" && int64(len(ro.Body)) <= s.cfg.IdempotencyMaxBodyBytes
	if idemKey != "" && !storeIdem {
		w.Header().Set("X-Idempotency-Disabled", "response_too_large")
	}
	w.WriteHeader(ro.AgentStatus)
	_, _ = w.Write(ro.Body)
	if idemKey != "" {
		if storeIdem {
			respHeaders := map[string]string{}
			for k, v := range ro.Headers {
				respHeaders[k] = v
			}
			respHeaders["X-Request-ID"] = requestIDFromContext(r.Context())
			respHeaders["X-Trace-ID"] = traceIDFromContext(r.Context())
			_ = s.cfg.DB.CompleteIdempotency(r.Context(), p.Sub, idemKey, ro.AgentStatus, respHeaders, ro.Body)
		} else {
			_ = s.cfg.DB.DeleteIdempotency(r.Context(), p.Sub, idemKey)
		}
	}
}

func (s *Server) handleAgentProxySSE(w http.ResponseWriter, r *http.Request, agentID, agentPath string) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	if ok, err := s.canAccessAgent(r.Context(), p, agentID); err != nil {
		writeErrorJSON(w, "db error", http.StatusInternalServerError)
		return
	} else if !ok {
		writeErrorJSON(w, "forbidden", http.StatusForbidden)
		return
	}
	deploymentID, derr := deploymentIDFromRequest(r)
	if derr != nil {
		writeErrorJSON(w, derr.Error(), http.StatusBadRequest)
		return
	}
	a, err := s.cfg.Registry.Require(agentID, deploymentID)
	if err != nil {
		writeErrorJSON(w, "agent not connected", http.StatusBadGateway)
		return
	}
	fl, ok := w.(http.Flusher)
	if !ok {
		writeErrorJSON(w, "streaming unsupported", http.StatusInternalServerError)
		return
	}

	body, err := readBodyBounded(r.Body, s.cfg.MaxRequestBodySize)
	if err != nil {
		writeErrorJSON(w, "request body too large", http.StatusRequestEntityTooLarge)
		return
	}

	headers := buildAgentForwardHeaders(r, p.Sub)

	streamID := newID()
	streamCh, err := a.RegisterStream(streamID)
	if err != nil {
		writeErrorJSON(w, "broker overloaded", http.StatusServiceUnavailable)
		return
	}
	defer a.CloseStream(streamID)

	sendCancel := func() {
		_ = a.SendAny(proto.StreamCancel{
			Type: proto.TypeHTTPStreamCancel,
			ID:   streamID,
		})
	}

	msg := proto.StreamRequest{
		Type: proto.TypeHTTPStreamRequest,
		ID:   streamID,
		Req: proto.HTTPRequest{
			Method:  r.Method,
			Path:    agentPath,
			Query:   r.URL.RawQuery,
			Headers: headers,
			BodyB64: base64.StdEncoding.EncodeToString(body),
		},
	}
	if err := a.SendStream(msg); err != nil {
		s.cfg.Registry.Delete(agentID, deploymentID)
		writeErrorJSON(w, "agent send failed", http.StatusBadGateway)
		return
	}

	// Wait for stream start (status + headers) before writing client response.
	timeout := 15 * time.Second
	if dl, ok := r.Context().Deadline(); ok {
		timeout = time.Until(dl)
	}
	startTimer := time.NewTimer(timeout)
	defer startTimer.Stop()

	var started bool
	for !started {
		select {
		case <-r.Context().Done():
			sendCancel()
			return
		case <-startTimer.C:
			sendCancel()
			writeErrorJSON(w, "agent stream start timeout", http.StatusGatewayTimeout)
			return
		case m, ok := <-streamCh:
			if !ok {
				writeErrorJSON(w, "agent disconnected", http.StatusBadGateway)
				return
			}
			ss, ok := m.(proto.StreamStart)
			if !ok {
				if sp, ok := m.(*proto.StreamStart); ok && sp != nil {
					ss = *sp
					ok = true
				}
			}
			if !ok {
				// Ignore until we get StreamStart.
				continue
			}
			for k, v := range ss.Resp.Headers {
				kl := strings.ToLower(k)
				if kl == "content-length" || kl == "connection" || kl == "transfer-encoding" {
					continue
				}
				w.Header().Set(k, v)
			}
			if strings.TrimSpace(w.Header().Get("Content-Type")) == "" {
				w.Header().Set("Content-Type", "text/event-stream")
			}
			w.Header().Set("Cache-Control", "no-cache")
			w.Header().Set("Connection", "keep-alive")
			w.WriteHeader(ss.Resp.Status)
			fl.Flush()
			started = true
		}
	}

	// Forward chunks until end, disconnect, or client cancel.
	for {
		select {
		case <-r.Context().Done():
			sendCancel()
			return
		case m, ok := <-streamCh:
			if !ok {
				return
			}
			switch v := m.(type) {
			case proto.StreamChunk:
				b, err := base64.StdEncoding.DecodeString(v.Data)
				if err != nil {
					sendCancel()
					return
				}
				if len(b) > 0 {
					if _, err := w.Write(b); err != nil {
						sendCancel()
						return
					}
					fl.Flush()
				}
			case *proto.StreamChunk:
				if v == nil {
					continue
				}
				b, err := base64.StdEncoding.DecodeString(v.Data)
				if err != nil {
					sendCancel()
					return
				}
				if len(b) > 0 {
					if _, err := w.Write(b); err != nil {
						sendCancel()
						return
					}
					fl.Flush()
				}
			case proto.StreamEnd, *proto.StreamEnd:
				return
			default:
				// ignore
			}
		}
	}
}
