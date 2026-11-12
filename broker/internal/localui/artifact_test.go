package localui

import (
	"net/http"
	"net/url"
	"strings"
	"testing"
)

func TestRawArtifactIsolation(t *testing.T) {
	s, key := fixture(t, func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", r.URL.Query().Get("type"))
		w.Header().Set("Content-Disposition", "inline; filename=artifact")
		w.Header().Set("Content-Security-Policy", "default-src *; script-src 'unsafe-inline'")
		_, _ = w.Write([]byte("artifact body"))
	})
	cookie := login(t, s, key)
	cases := []struct {
		name, mediaType, wantType string
		download                  bool
	}{
		{"svg", "image/svg+xml", "application/octet-stream", true},
		{"html", "text/html; charset=utf-8", "application/octet-stream", true},
		{"javascript", "application/javascript", "application/octet-stream", true},
		{"text-javascript", "text/javascript", "application/octet-stream", true},
		{"xml", "application/xml", "application/octet-stream", true},
		{"unknown", "application/x-custom", "application/octet-stream", true},
		{"missing", "", "application/octet-stream", true},
		{"malformed", "image/png; charset", "application/octet-stream", true},
		{"png", "image/png", "image/png", false},
		{"jpeg", "image/jpeg", "image/jpeg", false},
		{"plain-text", "text/plain; charset=utf-8", "text/plain; charset=utf-8", false},
		{"audio", "audio/mpeg", "audio/mpeg", false},
		{"video", "video/mp4", "video/mp4", false},
	}
	for _, route := range []string{"/api/v1/file", "/api/v1/blob"} {
		for _, tc := range cases {
			t.Run(strings.TrimPrefix(route, "/api/v1/")+"/"+tc.name, func(t *testing.T) {
				w := call(s, "GET", route+"?"+url.Values{"type": []string{tc.mediaType}}.Encode(), "", "", cookie)
				if w.Code != http.StatusOK || w.Body.String() != "artifact body" {
					t.Fatalf("artifact response changed: %d %q", w.Code, w.Body.String())
				}
				if got := w.Header().Get("Content-Type"); got != tc.wantType {
					t.Fatalf("Content-Type = %q, want %q", got, tc.wantType)
				}
				if got := w.Header().Get("Content-Disposition"); tc.download && got != "attachment" {
					t.Fatalf("active artifact must download, got %q", got)
				} else if !tc.download && got != "inline; filename=artifact" {
					t.Fatalf("safe preview disposition changed: %q", got)
				}
				csp := strings.Join(w.Header().Values("Content-Security-Policy"), "; ")
				if !strings.Contains(csp, "sandbox; default-src 'none'") || strings.Contains(csp, "unsafe-inline'") && strings.Contains(csp, "script-src 'unsafe-inline'") {
					t.Fatalf("artifact isolation CSP missing or upstream script policy retained: %q", csp)
				}
				if w.Header().Get("X-Content-Type-Options") != "nosniff" {
					t.Fatal("artifact MIME sniffing was not disabled")
				}
			})
		}
	}
	for _, route := range []string{"/api/v1/blob/meta", "/api/v1/session/artifacts"} {
		w := call(s, "GET", route+"?type=application%2Fjson", "", "", cookie)
		if w.Code != http.StatusOK || w.Header().Get("Content-Type") != "application/json" {
			t.Fatalf("JSON metadata response changed: %s %d %q", route, w.Code, w.Header().Get("Content-Type"))
		}
		if strings.Contains(strings.Join(w.Header().Values("Content-Security-Policy"), "; "), "sandbox;") || w.Header().Get("Content-Disposition") == "attachment" {
			t.Fatalf("artifact policy applied to metadata route %s", route)
		}
	}
	w := call(s, "GET", "/", "", "", cookie)
	if w.Code != http.StatusOK || !strings.HasPrefix(w.Header().Get("Content-Type"), "text/html") || strings.Contains(strings.Join(w.Header().Values("Content-Security-Policy"), "; "), "sandbox;") {
		t.Fatal("static UI document policy changed")
	}
}
