package transport

import "time"

// Conn is a transport-agnostic connector channel between the broker and agentd.
// It is intentionally narrow to keep it easy to implement across transports.
type Conn interface {
	ReadJSON(v any) error
	WriteJSON(v any) error
	ReadMessage() ([]byte, error)
	Close() error
	SetReadLimit(n int64)
	SetReadDeadline(t time.Time) error
	SetPongHandler(h func(string) error)
	Ping() error
}
