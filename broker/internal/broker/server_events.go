package broker

import (
	"encoding/json"
	"net/http"
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
