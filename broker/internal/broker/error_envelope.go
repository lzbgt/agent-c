package broker

import (
	"encoding/json"
	"net/http"
)

func writeJSON(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	b, _ := json.Marshal(v)
	_, _ = w.Write(b)
}

func errorCodeFromMessage(msg string) string {
	out := make([]rune, 0, len(msg))
	prevUS := false
	for _, r := range msg {
		switch {
		case r >= 'a' && r <= 'z':
			out = append(out, r)
			prevUS = false
		case r >= '0' && r <= '9':
			out = append(out, r)
			prevUS = false
		case r >= 'A' && r <= 'Z':
			out = append(out, r+('a'-'A'))
			prevUS = false
		default:
			if !prevUS && len(out) > 0 {
				out = append(out, '_')
				prevUS = true
			}
		}
	}
	for len(out) > 0 && out[len(out)-1] == '_' {
		out = out[:len(out)-1]
	}
	if len(out) == 0 {
		return "error"
	}
	return string(out)
}

func errorEnvelope(msg string, code ...string) map[string]any {
	finalCode := ""
	if len(code) > 0 && code[0] != "" {
		finalCode = code[0]
	} else {
		finalCode = errorCodeFromMessage(msg)
	}
	out := map[string]any{
		"ok":    false,
		"error": msg,
		"err":   msg,
	}
	if finalCode != "" {
		out["code"] = finalCode
	}
	return out
}

func writeErrorJSON(w http.ResponseWriter, msg string, status int, code ...string) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	b, _ := json.Marshal(errorEnvelope(msg, code...))
	_, _ = w.Write(b)
}
