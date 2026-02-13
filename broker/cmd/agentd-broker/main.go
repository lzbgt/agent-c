package main

import (
	"context"
	"crypto/tls"
	"flag"
	"log"
	"net/http"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
	"time"

	"agentd-broker/internal/auth"
	"agentd-broker/internal/broker"
	"agentd-broker/internal/config"
	"agentd-broker/internal/db"
	"agentd-broker/internal/events"
	"agentd-broker/internal/oidc"
	"agentd-broker/internal/registry"
)

func envInt64(key string) (int64, bool) {
	raw := strings.TrimSpace(os.Getenv(key))
	if raw == "" {
		return 0, false
	}
	n, err := strconv.ParseInt(raw, 10, 64)
	if err != nil {
		log.Printf("invalid %s: %v", key, err)
		return 0, false
	}
	return n, true
}

func envDurationMS(key string) (time.Duration, bool) {
	n, ok := envInt64(key)
	if !ok {
		return 0, false
	}
	if n < 0 {
		log.Printf("invalid %s: must be >= 0", key)
		return 0, false
	}
	return time.Duration(n) * time.Millisecond, true
}

func envBool(key string) (bool, bool) {
	raw := strings.TrimSpace(os.Getenv(key))
	if raw == "" {
		return false, false
	}
	switch strings.ToLower(raw) {
	case "1", "true", "yes", "y", "on":
		return true, true
	case "0", "false", "no", "n", "off":
		return false, true
	default:
		log.Printf("invalid %s: must be boolean", key)
		return false, false
	}
}

func main() {
	maxBodyDefault := int64(64 * 1024 * 1024)
	if v, ok := envInt64("AGENTD_BROKER_MAX_BODY_BYTES"); ok && v > 0 {
		maxBodyDefault = v
	}
	maxHeaderBytesDefault := 1 << 20
	if v, ok := envInt64("AGENTD_BROKER_MAX_HEADER_BYTES"); ok && v > 0 && v < (1<<30) {
		maxHeaderBytesDefault = int(v)
	}
	readTimeoutDefault := time.Duration(0)
	if v, ok := envDurationMS("AGENTD_BROKER_READ_TIMEOUT_MS"); ok {
		readTimeoutDefault = v
	}
	writeTimeoutDefault := time.Duration(0)
	if v, ok := envDurationMS("AGENTD_BROKER_WRITE_TIMEOUT_MS"); ok {
		writeTimeoutDefault = v
	}
	idleTimeoutDefault := 120 * time.Second
	if v, ok := envDurationMS("AGENTD_BROKER_IDLE_TIMEOUT_MS"); ok {
		idleTimeoutDefault = v
	}
	readHeaderTimeoutDefault := 10 * time.Second
	if v, ok := envDurationMS("AGENTD_BROKER_READ_HEADER_TIMEOUT_MS"); ok {
		readHeaderTimeoutDefault = v
	}

	var listen = flag.String("listen", ":8443", "listen address")
	var tlsCert = flag.String("tls-cert", "", "TLS server cert PEM path")
	var tlsKey = flag.String("tls-key", "", "TLS server private key PEM path")
	var tlsClientCA = flag.String("tls-client-ca", "", "client CA PEM to verify agent mTLS (optional)")
	var requireAgentMTLS = flag.Bool("require-agent-mtls", true, "require verified client cert for agent connect")
	var agentCNPfx = flag.String("agent-cn-prefix", "agentd-", "agent mTLS client cert CN prefix (e.g. agentd-)")

	var dbDSN = flag.String("db-dsn", "", "postgres dsn (or env AGENTD_BROKER_DB_DSN / DATABASE_URL)")
	var oidcIssuer = flag.String("oidc-issuer", "", "OIDC issuer URL (required)")
	var oidcAudience = flag.String("oidc-audience", "", "OIDC audience / client_id (required)")
	var clientAuthFile = flag.String("client-auth-file", "", "path to JSON client auth file (optional; env AGENTD_BROKER_CLIENT_AUTH_FILE)")
	var clientAuthFallback = flag.Bool("client-auth-fallback", false, "allow client auth tokens when OIDC auth fails")
	var clientAuthReloadMS = flag.Int64("client-auth-reload-ms", 0, "reload client auth file interval in ms (0 disables; env AGENTD_BROKER_CLIENT_AUTH_RELOAD_MS)")
	var clientAuthStrict = flag.Bool("client-auth-strict", false, "fail readiness if client auth reload fails (env AGENTD_BROKER_CLIENT_AUTH_STRICT)")
	var adminSubsCSV = flag.String("admin-subs", "", "comma-separated OIDC sub values treated as admin")
	var corsOriginsCSV = flag.String("cors-origins", "", "comma-separated allowed CORS origins (e.g. https://ui.example.com)")
	var maxPendingPerAgent = flag.Int("max-pending-per-agent", 256, "max pending proxied requests per agent (0=unlimited)")
	var maxStreamsPerAgent = flag.Int("max-streams-per-agent", 64, "max active streams per agent (0=unlimited)")
	var maxBodyBytes = flag.Int64("max-body-bytes", maxBodyDefault, "max request body bytes (default: 64MiB)")
	var maxHeaderBytes = flag.Int("max-header-bytes", maxHeaderBytesDefault, "max HTTP request header bytes")
	var readTimeout = flag.Duration("read-timeout", readTimeoutDefault, "HTTP server read timeout (0 disables)")
	var writeTimeout = flag.Duration("write-timeout", writeTimeoutDefault, "HTTP server write timeout (0 disables; keep 0 for SSE)")
	var idleTimeout = flag.Duration("idle-timeout", idleTimeoutDefault, "HTTP server idle timeout")
	var readHeaderTimeout = flag.Duration("read-header-timeout", readHeaderTimeoutDefault, "HTTP server read header timeout")
	var sseKeepalive = flag.Duration("sse-keepalive", 15*time.Second, "SSE keepalive comment interval")
	var readyCache = flag.Duration("ready-cache", 5*time.Second, "readiness check cache interval")
	var shutdownTimeout = flag.Duration("shutdown-timeout", 15*time.Second, "graceful shutdown timeout")

	flag.Parse()

	dsn := strings.TrimSpace(*dbDSN)
	if dsn == "" {
		dsn = strings.TrimSpace(os.Getenv("AGENTD_BROKER_DB_DSN"))
	}
	if dsn == "" {
		dsn = strings.TrimSpace(os.Getenv("DATABASE_URL"))
	}
	if dsn == "" {
		log.Fatalf("missing db dsn: set --db-dsn or env AGENTD_BROKER_DB_DSN / DATABASE_URL")
	}

	clientAuthPath := strings.TrimSpace(*clientAuthFile)
	if clientAuthPath == "" {
		clientAuthPath = strings.TrimSpace(os.Getenv("AGENTD_BROKER_CLIENT_AUTH_FILE"))
	}
	var clientAuth *auth.ClientAuth
	if clientAuthPath != "" {
		ca, err := config.LoadClientAuthFromFile(clientAuthPath)
		if err != nil {
			log.Fatalf("client auth load failed: %v", err)
		}
		clientAuth = ca
	}

	fallbackEnabled := *clientAuthFallback
	if v, ok := envBool("AGENTD_BROKER_CLIENT_AUTH_FALLBACK"); ok && v {
		fallbackEnabled = true
	}
	reloadMS := *clientAuthReloadMS
	if v, ok := envInt64("AGENTD_BROKER_CLIENT_AUTH_RELOAD_MS"); ok && v > 0 {
		reloadMS = v
	}
	strictEnabled := *clientAuthStrict
	if v, ok := envBool("AGENTD_BROKER_CLIENT_AUTH_STRICT"); ok && v {
		strictEnabled = true
	}

	iss := strings.TrimSpace(*oidcIssuer)
	aud := strings.TrimSpace(*oidcAudience)
	hasOIDC := iss != "" || aud != ""
	if hasOIDC {
		if iss == "" || aud == "" {
			log.Fatalf("missing oidc config: set --oidc-issuer and --oidc-audience")
		}
	}
	if !hasOIDC && clientAuth == nil {
		log.Fatalf("missing auth config: set --oidc-issuer/--oidc-audience or --client-auth-file")
	}
	var ver *oidc.Verifier
	if hasOIDC {
		ver = &oidc.Verifier{IssuerURL: iss, Audience: aud}
	}

	tlsCfg := (*tls.Config)(nil)
	if strings.TrimSpace(*tlsCert) != "" || strings.TrimSpace(*tlsKey) != "" {
		if strings.TrimSpace(*tlsCert) == "" || strings.TrimSpace(*tlsKey) == "" {
			log.Fatalf("--tls-cert and --tls-key must be set together")
		}
		cfg, err := auth.ConfigureServerTLS(*tlsCert, *tlsKey, *tlsClientCA)
		if err != nil {
			log.Fatalf("tls config failed: %v", err)
		}
		tlsCfg = cfg
	}

	// Postgres can take a few seconds to become ready (especially under docker compose).
	// Treat initial connection failures as transient and retry with backoff.
	var dbConn *db.DB
	{
		ctx := context.Background()
		deadline := time.Now().Add(90 * time.Second)
		sleep := 250 * time.Millisecond
		for {
			c, err := db.Open(ctx, dsn)
			if err == nil {
				dbConn = c
				break
			}
			if time.Now().After(deadline) {
				log.Fatalf("db open failed (timeout): %v", err)
			}
			log.Printf("db open failed (retrying): %v", err)
			time.Sleep(sleep)
			if sleep < 4*time.Second {
				sleep *= 2
			}
		}
	}
	defer dbConn.Close()
	if err := dbConn.Migrate(context.Background()); err != nil {
		log.Fatalf("db migrate failed: %v", err)
	}

	reg := registry.New()
	ev := events.New()
	adminSubs := map[string]bool{}
	for _, part := range strings.Split(*adminSubsCSV, ",") {
		sub := strings.TrimSpace(part)
		if sub != "" {
			adminSubs[sub] = true
		}
	}
	allowedOrigins := []string{}
	for _, part := range strings.Split(*corsOriginsCSV, ",") {
		o := strings.TrimSpace(part)
		if o != "" {
			allowedOrigins = append(allowedOrigins, o)
		}
	}
	s, err := broker.New(broker.Config{
		OIDC:               ver,
		ClientAuth:         clientAuth,
		ClientAuthFallback: fallbackEnabled,
		ClientAuthStrict:   strictEnabled,
		DB:                 dbConn,
		Registry:           reg,
		Events:             ev,
		AgentCNPfx:         *agentCNPfx,
		RequireAgentMTLS:   *requireAgentMTLS,
		MaxRequestBodySize: func() int64 {
			if *maxBodyBytes <= 0 {
				return 64 * 1024 * 1024
			}
			return *maxBodyBytes
		}(),
		MaxPendingPerAgent: *maxPendingPerAgent,
		MaxStreamsPerAgent: *maxStreamsPerAgent,
		AllowedOrigins:     allowedOrigins,
		SSEKeepaliveInterval: func() time.Duration {
			if *sseKeepalive <= 0 {
				return 15 * time.Second
			}
			return *sseKeepalive
		}(),
		ReadinessCacheInterval: func() time.Duration {
			if *readyCache <= 0 {
				return 5 * time.Second
			}
			return *readyCache
		}(),
		AdminSubs: adminSubs,
	})
	if err != nil {
		log.Fatalf("broker init failed: %v", err)
	}
	if clientAuth != nil {
		s.SetClientAuthStatus(true, "")
	}

	srv := &http.Server{
		Addr:              *listen,
		Handler:           s.Handler(),
		TLSConfig:         tlsCfg,
		ReadTimeout:       *readTimeout,
		WriteTimeout:      *writeTimeout,
		ReadHeaderTimeout: *readHeaderTimeout,
		IdleTimeout:       *idleTimeout,
		MaxHeaderBytes: func() int {
			if *maxHeaderBytes <= 0 {
				return 1 << 20
			}
			return *maxHeaderBytes
		}(),
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	errCh := make(chan error, 1)
	go func() {
		if tlsCfg != nil {
			errCh <- srv.ListenAndServeTLS(strings.TrimSpace(*tlsCert), strings.TrimSpace(*tlsKey))
			return
		}
		errCh <- srv.ListenAndServe()
	}()

	if tlsCfg != nil {
		log.Printf("agentd-broker listening on https://%s", *listen)
		log.Printf("agent mTLS client CA set: %v", strings.TrimSpace(*tlsClientCA) != "")
		log.Printf("agent mTLS required: %v", *requireAgentMTLS)
		log.Printf("agent CN prefix: %s", *agentCNPfx)
	} else {
		log.Printf("agentd-broker listening on http://%s (INSECURE)", *listen)
	}
	log.Printf("postgres dsn set: %v", dsn != "")
	if hasOIDC {
		log.Printf("oidc issuer: %s", iss)
		log.Printf("oidc audience: %s", aud)
	} else {
		log.Printf("oidc disabled")
	}
	log.Printf("client auth configured: %v", clientAuth != nil)
	log.Printf("client auth fallback: %v", fallbackEnabled)
	log.Printf("client auth strict: %v", strictEnabled)
	log.Printf("cors origins configured: %v", len(allowedOrigins) > 0)
	log.Printf(
		"limits: max_pending_per_agent=%d max_streams_per_agent=%d max_body_bytes=%d max_header_bytes=%d",
		*maxPendingPerAgent,
		*maxStreamsPerAgent,
		*maxBodyBytes,
		*maxHeaderBytes,
	)
	log.Printf(
		"http timeouts: read=%s write=%s idle=%s read_header=%s",
		(*readTimeout).String(),
		(*writeTimeout).String(),
		(*idleTimeout).String(),
		(*readHeaderTimeout).String(),
	)
	reloadClientAuth := func(reason string) {
		if clientAuthPath == "" {
			log.Printf("client auth reload requested but no client auth file configured")
			return
		}
		ca, err := config.LoadClientAuthFromFile(clientAuthPath)
		if err != nil {
			s.SetClientAuthStatus(false, err.Error())
			log.Printf("client auth reload failed (%s): %v", reason, err)
			return
		}
		s.SetClientAuth(ca)
		s.SetClientAuthStatus(true, "")
		log.Printf("client auth reloaded (%s)", reason)
	}
	if clientAuthPath != "" {
		hupCh := make(chan os.Signal, 1)
		signal.Notify(hupCh, syscall.SIGHUP)
		go func() {
			for range hupCh {
				reloadClientAuth("sighup")
			}
		}()
	}
	if reloadMS > 0 {
		if clientAuthPath == "" {
			log.Printf("client auth reload requested but no client auth file configured; reload disabled")
		} else {
			if reloadMS < 1000 {
				log.Printf("client auth reload interval too low (%dms); clamping to 1000ms", reloadMS)
				reloadMS = 1000
			}
			interval := time.Duration(reloadMS) * time.Millisecond
			log.Printf("client auth reload enabled: every %s", interval)
			ticker := time.NewTicker(interval)
			go func() {
				for range ticker.C {
					reloadClientAuth("interval")
				}
			}()
		}
	}

	select {
	case <-ctx.Done():
		st := *shutdownTimeout
		if st <= 0 {
			st = 15 * time.Second
		}
		sctx, cancel := context.WithTimeout(context.Background(), st)
		_ = srv.Shutdown(sctx)
		cancel()
		err := <-errCh
		if err != nil && err != http.ErrServerClosed {
			log.Fatalf("server error after shutdown: %v", err)
		}
	case err := <-errCh:
		if err != nil && err != http.ErrServerClosed {
			log.Fatal(err)
		}
	}

}
