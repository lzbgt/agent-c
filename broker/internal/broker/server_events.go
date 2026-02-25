package broker

import (
	"encoding/json"
	"net/http"
	"strings"
	"time"
)

func (s *Server) handleEventsSSE(w http.ResponseWriter, r *http.Request) {
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	fl, ok := w.(http.Flusher)
	if !ok {
		writeErrorJSON(w, "streaming unsupported", http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("Connection", "keep-alive")
	// Helps Nginx/Envoy deployments keep SSE streaming rather than buffering.
	w.Header().Set("X-Accel-Buffering", "no")

	ch, cancel := s.cfg.Events.Subscribe(p.Sub)
	defer cancel()

	if _, err := w.Write([]byte(":ok\n\n")); err != nil {
		return
	}
	fl.Flush()

	keepAlive := s.cfg.SSEKeepaliveInterval
	if keepAlive <= 0 {
		keepAlive = 15 * time.Second
	}
	t := time.NewTicker(keepAlive)
	defer t.Stop()

	for {
		select {
		case <-r.Context().Done():
			return
		case <-t.C:
			if _, err := w.Write([]byte(":keepalive\n\n")); err != nil {
				return
			}
			fl.Flush()
		case ev, ok := <-ch:
			if !ok {
				return
			}
			b, _ := json.Marshal(ev)
			// Minimal SSE framing. Clients can decode JSON from data.
			if _, err := w.Write([]byte("event: " + ev.Type + "\n")); err != nil {
				return
			}
			if _, err := w.Write([]byte("data: ")); err != nil {
				return
			}
			if _, err := w.Write(b); err != nil {
				return
			}
			if _, err := w.Write([]byte("\n\n")); err != nil {
				return
			}
			fl.Flush()
		}
	}
}

func (s *Server) handleEventsReplay(w http.ResponseWriter, r *http.Request) {
	if r.Method != "GET" {
		writeErrorJSON(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	p, err := s.requirePrincipal(r)
	if err != nil {
		writeErrorJSON(w, err.Error(), http.StatusUnauthorized)
		return
	}
	limit := 200
	if v := strings.TrimSpace(r.URL.Query().Get("limit")); v != "" {
		if n, ok := parseIntBounded(v, 1, 1000); ok {
			limit = n
		}
	}
	sinceTS := int64(0)
	if v := strings.TrimSpace(r.URL.Query().Get("since_ts")); v != "" {
		if n, ok := parseInt64Bounded(v, 0, 9_999_999_999_999); ok {
			sinceTS = n
		}
	}
	var types []string
	if v := strings.TrimSpace(r.URL.Query().Get("types")); v != "" {
		for _, part := range strings.Split(v, ",") {
			part = strings.TrimSpace(part)
			if part != "" {
				types = append(types, part)
			}
		}
	}
	rows, err := s.cfg.DB.ListBrokerEvents(r.Context(), p.Sub, sinceTS, limit, types)
	if err != nil {
		writeErrorJSON(w, "db error", http.StatusInternalServerError)
		return
	}
	events := make([]json.RawMessage, 0, len(rows))
	nextTS := sinceTS
	for _, row := range rows {
		if len(row.JSON) == 0 {
			continue
		}
		events = append(events, row.JSON)
		if row.TSUnixMS > nextTS {
			nextTS = row.TSUnixMS
		}
	}
	writeJSON(w, map[string]any{
		"ok":            true,
		"user_sub":      p.Sub,
		"since_ts":      sinceTS,
		"next_since_ts": nextTS,
		"limit":         limit,
		"count":         len(events),
		"events":        events,
	})
}

func parseInt64Bounded(raw string, lo, hi int64) (int64, bool) {
	raw = strings.TrimSpace(raw)
	if raw == "" {
		return 0, false
	}
	var n int64
	for _, r := range raw {
		if r < '0' || r > '9' {
			return 0, false
		}
		n = n*10 + int64(r-'0')
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
