package proto

import (
	"encoding/json"
)

const (
	TypeHello       = "hello"
	TypeHelloAck    = "hello_ack"
	TypeHTTPRequest = "http_request"
	TypeHTTPResp    = "http_response"

	TypeHTTPStreamRequest = "http_stream_request"
	TypeHTTPStreamStart   = "http_stream_start"
	TypeHTTPStreamChunk   = "http_stream_chunk"
	TypeHTTPStreamEnd     = "http_stream_end"
	TypeHTTPStreamCancel  = "http_stream_cancel"
)

type Hello struct {
	Type    string                     `json:"type"`
	AgentID string                     `json:"agent_id"`
	Meta    map[string]any             `json:"meta,omitempty"`
	Labels  map[string]string          `json:"labels,omitempty"`
	Extra   map[string]json.RawMessage `json:"-"`
}

type HelloAck struct {
	Type    string `json:"type"`
	OK      bool   `json:"ok"`
	AgentID string `json:"agent_id,omitempty"`
	Error   string `json:"error,omitempty"`
}

type HTTPRequest struct {
	Method  string            `json:"method"`
	Path    string            `json:"path"`
	Query   string            `json:"query,omitempty"`
	Headers map[string]string `json:"headers,omitempty"`
	BodyB64 string            `json:"body_b64,omitempty"`
}

type HTTPResponse struct {
	Status  int               `json:"status"`
	Headers map[string]string `json:"headers,omitempty"`
	BodyB64 string            `json:"body_b64,omitempty"`
}

type RelayRequest struct {
	Type string      `json:"type"`
	ID   string      `json:"id"`
	Req  HTTPRequest `json:"req"`
}

type RelayResponse struct {
	Type string       `json:"type"`
	ID   string       `json:"id"`
	Resp HTTPResponse `json:"resp"`
	Err  string       `json:"err,omitempty"`
}

// Streaming proxy messages (SSE/long responses).
type StreamRequest struct {
	Type string      `json:"type"`
	ID   string      `json:"id"`
	Req  HTTPRequest `json:"req"`
}

type StreamStart struct {
	Type string       `json:"type"`
	ID   string       `json:"id"`
	Resp HTTPResponse `json:"resp"`
}

type StreamChunk struct {
	Type string `json:"type"`
	ID   string `json:"id"`
	Data string `json:"data_b64"`
}

type StreamEnd struct {
	Type string `json:"type"`
	ID   string `json:"id"`
	Err  string `json:"err,omitempty"`
}

type StreamCancel struct {
	Type string `json:"type"`
	ID   string `json:"id"`
}
