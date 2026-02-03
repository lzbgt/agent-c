package main

import (
	"context"
	"crypto/tls"
	"flag"
	"log"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"agentd-broker/internal/auth"
	"agentd-broker/internal/broker"
	"agentd-broker/internal/db"
	"agentd-broker/internal/events"
	"agentd-broker/internal/oidc"
	"agentd-broker/internal/registry"
)

func main() {
	var listen = flag.String("listen", ":8443", "listen address")
	var tlsCert = flag.String("tls-cert", "", "TLS server cert PEM path")
	var tlsKey = flag.String("tls-key", "", "TLS server private key PEM path")
	var tlsClientCA = flag.String("tls-client-ca", "", "client CA PEM to verify agent mTLS (optional)")
	var requireAgentMTLS = flag.Bool("require-agent-mtls", true, "require verified client cert for agent connect")
	var agentCNPfx = flag.String("agent-cn-prefix", "agentd-", "agent mTLS client cert CN prefix (e.g. agentd-)")

	var dbDSN = flag.String("db-dsn", "", "postgres dsn (or env AGENTD_BROKER_DB_DSN / DATABASE_URL)")
	var oidcIssuer = flag.String("oidc-issuer", "", "OIDC issuer URL (required)")
	var oidcAudience = flag.String("oidc-audience", "", "OIDC audience / client_id (required)")
	var adminSubsCSV = flag.String("admin-subs", "", "comma-separated OIDC sub values treated as admin")
	var corsOriginsCSV = flag.String("cors-origins", "", "comma-separated allowed CORS origins (e.g. https://ui.example.com)")
	var maxPendingPerAgent = flag.Int("max-pending-per-agent", 256, "max pending proxied requests per agent (0=unlimited)")
	var maxStreamsPerAgent = flag.Int("max-streams-per-agent", 64, "max active streams per agent (0=unlimited)")
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

	iss := strings.TrimSpace(*oidcIssuer)
	aud := strings.TrimSpace(*oidcAudience)
	if iss == "" || aud == "" {
		log.Fatalf("missing oidc config: set --oidc-issuer and --oidc-audience")
	}
	ver := &oidc.Verifier{IssuerURL: iss, Audience: aud}

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
		DB:                 dbConn,
		Registry:           reg,
		Events:             ev,
		AgentCNPfx:         *agentCNPfx,
		RequireAgentMTLS:   *requireAgentMTLS,
		MaxRequestBodySize: 64 * 1024 * 1024,
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

	srv := &http.Server{
		Addr:              *listen,
		Handler:           s.Handler(),
		TLSConfig:         tlsCfg,
		ReadHeaderTimeout: 10 * time.Second,
		IdleTimeout:       120 * time.Second,
		MaxHeaderBytes:    1 << 20,
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
	log.Printf("oidc issuer: %s", iss)
	log.Printf("oidc audience: %s", aud)
	log.Printf("cors origins configured: %v", len(allowedOrigins) > 0)
	log.Printf("limits: max_pending_per_agent=%d max_streams_per_agent=%d", *maxPendingPerAgent, *maxStreamsPerAgent)

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
