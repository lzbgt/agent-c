package transport

import (
	"time"

	"github.com/gorilla/websocket"
)

// WebSocketConn adapts *websocket.Conn to the transport.Conn interface.
type WebSocketConn struct {
	conn *websocket.Conn
}

func NewWebSocket(conn *websocket.Conn) *WebSocketConn {
	return &WebSocketConn{conn: conn}
}

func (w *WebSocketConn) ReadJSON(v any) error {
	return w.conn.ReadJSON(v)
}

func (w *WebSocketConn) WriteJSON(v any) error {
	return w.conn.WriteJSON(v)
}

func (w *WebSocketConn) ReadMessage() ([]byte, error) {
	_, raw, err := w.conn.ReadMessage()
	return raw, err
}

func (w *WebSocketConn) Close() error {
	return w.conn.Close()
}

func (w *WebSocketConn) SetReadLimit(n int64) {
	w.conn.SetReadLimit(n)
}

func (w *WebSocketConn) SetReadDeadline(t time.Time) error {
	return w.conn.SetReadDeadline(t)
}

func (w *WebSocketConn) SetPongHandler(h func(string) error) {
	w.conn.SetPongHandler(h)
}

func (w *WebSocketConn) Ping() error {
	return w.conn.WriteControl(websocket.PingMessage, []byte("ping"), time.Now().Add(5*time.Second))
}
