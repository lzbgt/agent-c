package broker

import (
	"bufio"
	"context"
	"errors"
	"io"
	"log"
	"net"
	"net/http"
	"regexp"
	"strings"
	"time"
)

type ctxKey int

const requestIDKey ctxKey = 1
const traceIDKey ctxKey = 2

func withRequestID(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		rid := strings.TrimSpace(r.Header.Get("X-Request-ID"))
		if rid == "" {
			rid = newID()
		}
		w.Header().Set("X-Request-ID", rid)
		ctx := context.WithValue(r.Context(), requestIDKey, rid)
		next.ServeHTTP(w, r.WithContext(ctx))
	})
}

func requestIDFromContext(ctx context.Context) string {
	if ctx == nil {
		return ""
	}
	v := ctx.Value(requestIDKey)
	s, _ := v.(string)
	return s
}

func isSafeTraceID(s string) bool {
	s = strings.TrimSpace(s)
	if s == "" || len(s) > 128 {
		return false
	}
	for _, r := range s {
		ok := (r >= 'a' && r <= 'z') ||
			(r >= 'A' && r <= 'Z') ||
			(r >= '0' && r <= '9') ||
			r == '-' || r == '_' || r == '.' || r == ':' || r == '@'
		if !ok {
			return false
		}
	}
	return true
}

func withTraceID(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		tid := strings.TrimSpace(r.Header.Get("X-Trace-ID"))
		if !isSafeTraceID(tid) {
			tid = ""
		}
		if tid == "" {
			// Default to X-Request-ID so the broker can always correlate logs and SSE.
			tid = requestIDFromContext(r.Context())
		}
		if tid == "" {
			tid = newID()
		}
		w.Header().Set("X-Trace-ID", tid)
		ctx := context.WithValue(r.Context(), traceIDKey, tid)
		next.ServeHTTP(w, r.WithContext(ctx))
	})
}

func traceIDFromContext(ctx context.Context) string {
	if ctx == nil {
		return ""
	}
	v := ctx.Value(traceIDKey)
	s, _ := v.(string)
	return s
}

type statusRecorder struct {
	http.ResponseWriter
	status int
	bytes  int64
}

func (rw *statusRecorder) WriteHeader(code int) {
	rw.status = code
	rw.ResponseWriter.WriteHeader(code)
}

func (rw *statusRecorder) Write(p []byte) (int, error) {
	if rw.status == 0 {
		rw.status = http.StatusOK
	}
	n, err := rw.ResponseWriter.Write(p)
	rw.bytes += int64(n)
	return n, err
}

func (rw *statusRecorder) Flush() {
	if f, ok := rw.ResponseWriter.(http.Flusher); ok {
		f.Flush()
	}
}

func (rw *statusRecorder) Hijack() (net.Conn, *bufio.ReadWriter, error) {
	h, ok := rw.ResponseWriter.(http.Hijacker)
	if !ok {
		return nil, nil, errors.New("hijacker not supported")
	}
	return h.Hijack()
}

func (rw *statusRecorder) Push(target string, opts *http.PushOptions) error {
	p, ok := rw.ResponseWriter.(http.Pusher)
	if !ok {
		return http.ErrNotSupported
	}
	return p.Push(target, opts)
}

func (rw *statusRecorder) ReadFrom(r io.Reader) (int64, error) {
	// Preserve optimized io.Copy paths when supported by the underlying writer.
	if rf, ok := rw.ResponseWriter.(io.ReaderFrom); ok {
		if rw.status == 0 {
			rw.status = http.StatusOK
		}
		n, err := rf.ReadFrom(r)
		rw.bytes += n
		return n, err
	}
	return io.Copy(rw, r)
}

func withAccessLog(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		sr := &statusRecorder{ResponseWriter: w}
		next.ServeHTTP(sr, r)
		dur := time.Since(start)
		status := sr.status
		if status == 0 {
			status = http.StatusOK
		}
		rid := requestIDFromContext(r.Context())
		tid := traceIDFromContext(r.Context())
		log.Printf("broker http method=%s path=%s status=%d bytes=%d dur_ms=%d remote=%s rid=%s trace_id=%s",
			r.Method,
			r.URL.Path,
			status,
			sr.bytes,
			int(dur.Milliseconds()),
			strings.TrimSpace(r.RemoteAddr),
			rid,
			tid,
		)
	})
}

func withRecovery(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		defer func() {
			if rec := recover(); rec != nil {
				rid := requestIDFromContext(r.Context())
				log.Printf("broker panic rid=%s err=%v", rid, rec)
				writeErrorJSON(w, "internal server error", http.StatusInternalServerError)
			}
		}()
		next.ServeHTTP(w, r)
	})
}

type CorsRoute struct {
	PathPrefix       string
	Origins          []string
	AllowHeaders     string
	AllowMethods     string
	AllowCredentials *bool
	MaxAgeSeconds    int
}

type CorsConfig struct {
	Origins          []string
	AllowHeaders     string
	AllowMethods     string
	AllowCredentials bool
	MaxAgeSeconds    int
	Routes           []CorsRoute
}

type corsOriginMatcher struct {
	any   bool
	exact map[string]bool
	regex []*regexp.Regexp
}

func buildOriginMatcher(patterns []string) corsOriginMatcher {
	m := corsOriginMatcher{exact: map[string]bool{}}
	for _, raw := range patterns {
		p := strings.TrimSpace(raw)
		if p == "" {
			continue
		}
		if p == "*" {
			m.any = true
			continue
		}
		if strings.HasPrefix(p, "re:") {
			reSrc := strings.TrimSpace(strings.TrimPrefix(p, "re:"))
			if reSrc == "" {
				continue
			}
			re, err := regexp.Compile(reSrc)
			if err != nil {
				log.Printf("invalid cors origin regex: %q err=%v", reSrc, err)
				continue
			}
			m.regex = append(m.regex, re)
			continue
		}
		m.exact[strings.ToLower(p)] = true
	}
	return m
}

func (m corsOriginMatcher) match(origin string) (bool, bool, bool) {
	origin = strings.TrimSpace(origin)
	if origin == "" {
		return false, false, false
	}
	if m.exact[strings.ToLower(origin)] {
		return true, true, false
	}
	for _, re := range m.regex {
		if re.MatchString(origin) {
			return true, false, true
		}
	}
	if m.any {
		return true, false, false
	}
	return false, false, false
}

type corsRuleCompiled struct {
	PathPrefix       string
	Matcher          corsOriginMatcher
	AllowHeaders     string
	AllowMethods     string
	AllowCredentials *bool
	MaxAgeSeconds    int
	OriginsProvided  bool
}

func compileCorsRules(routes []CorsRoute) []corsRuleCompiled {
	out := make([]corsRuleCompiled, 0, len(routes))
	for _, r := range routes {
		originsProvided := r.Origins != nil
		out = append(out, corsRuleCompiled{
			PathPrefix:       strings.TrimSpace(r.PathPrefix),
			Matcher:          buildOriginMatcher(r.Origins),
			AllowHeaders:     strings.TrimSpace(r.AllowHeaders),
			AllowMethods:     strings.TrimSpace(r.AllowMethods),
			AllowCredentials: r.AllowCredentials,
			MaxAgeSeconds:    r.MaxAgeSeconds,
			OriginsProvided:  originsProvided,
		})
	}
	return out
}

func selectCorsRule(path string, rules []corsRuleCompiled) *corsRuleCompiled {
	if len(rules) == 0 {
		return nil
	}
	best := (*corsRuleCompiled)(nil)
	bestLen := -1
	for i := range rules {
		pfx := rules[i].PathPrefix
		if pfx == "" || strings.HasPrefix(path, pfx) {
			if len(pfx) > bestLen {
				bestLen = len(pfx)
				best = &rules[i]
			}
		}
	}
	return best
}

func withCORS(cfg CorsConfig, next http.Handler) http.Handler {
	hasDefaults := len(cfg.Origins) > 0 || strings.TrimSpace(cfg.AllowHeaders) != "" || strings.TrimSpace(cfg.AllowMethods) != "" || cfg.AllowCredentials
	if len(cfg.Routes) == 0 && !hasDefaults {
		return next
	}
	defaultMatcher := buildOriginMatcher(cfg.Origins)
	rules := compileCorsRules(cfg.Routes)
	defaultAllowHeaders := strings.TrimSpace(cfg.AllowHeaders)
	defaultAllowMethods := strings.TrimSpace(cfg.AllowMethods)
	defaultMaxAge := cfg.MaxAgeSeconds
	if defaultMaxAge <= 0 {
		defaultMaxAge = 600
	}

	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		origin := strings.TrimSpace(r.Header.Get("Origin"))
		rule := selectCorsRule(r.URL.Path, rules)

		policyMatcher := defaultMatcher
		allowHeaders := defaultAllowHeaders
		allowMethods := defaultAllowMethods
		allowCredentials := cfg.AllowCredentials
		maxAge := defaultMaxAge

		if rule != nil {
			if rule.OriginsProvided {
				policyMatcher = rule.Matcher
			}
			if rule.AllowHeaders != "" {
				allowHeaders = rule.AllowHeaders
			}
			if rule.AllowMethods != "" {
				allowMethods = rule.AllowMethods
			}
			if rule.AllowCredentials != nil {
				allowCredentials = *rule.AllowCredentials
			}
			if rule.MaxAgeSeconds > 0 {
				maxAge = rule.MaxAgeSeconds
			}
		}

		allowedOrigin := ""
		if origin != "" {
			if ok, exact, regex := policyMatcher.match(origin); ok {
				switch {
				case exact || regex:
					allowedOrigin = origin
				default:
					if allowCredentials {
						allowedOrigin = origin
					} else {
						allowedOrigin = "*"
					}
				}
			}
		}

		if allowedOrigin != "" {
			w.Header().Set("Access-Control-Allow-Origin", allowedOrigin)
			if allowCredentials {
				w.Header().Set("Access-Control-Allow-Credentials", "true")
			}
			if allowHeaders != "" {
				w.Header().Set("Access-Control-Allow-Headers", allowHeaders)
			}
			if allowMethods != "" {
				w.Header().Set("Access-Control-Allow-Methods", allowMethods)
			}
			if maxAge > 0 {
				w.Header().Set("Access-Control-Max-Age", itoa(maxAge))
			}
			w.Header().Set("Access-Control-Expose-Headers", "X-Request-ID, X-Trace-ID, X-Idempotency-Replay, X-Idempotency-Key, X-Idempotency-Disabled")
			if allowedOrigin != "*" {
				w.Header().Add("Vary", "Origin")
			}
		} else if origin != "" && r.Method == http.MethodOptions {
			writeErrorJSON(w, "origin not allowed", http.StatusForbidden)
			return
		}

		if r.Method == http.MethodOptions {
			w.WriteHeader(http.StatusNoContent)
			return
		}
		next.ServeHTTP(w, r)
	})
}
