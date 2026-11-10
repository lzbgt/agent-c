package main

import (
	"agentd-broker/internal/gateway"
	"context"
	"flag"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"
)

func main() {
	configFile := flag.String("config", "", "Owner-only gateway configuration JSON")
	flag.Parse()
	cfg, err := gateway.LoadConfig(*configFile)
	if err != nil {
		log.Fatal(err)
	}
	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()
	setup, stop := context.WithTimeout(ctx, 15*time.Second)
	app, err := gateway.New(setup, cfg)
	stop()
	if err != nil {
		log.Fatal("gateway setup failed: ", err)
	}
	defer app.Close()
	srv := &http.Server{Addr: cfg.Listen, Handler: app, ReadHeaderTimeout: 10 * time.Second, IdleTimeout: 60 * time.Second, MaxHeaderBytes: 32 << 10}
	go func() {
		<-ctx.Done()
		shutdown, done := context.WithTimeout(context.Background(), 10*time.Second)
		defer done()
		_ = srv.Shutdown(shutdown)
	}()
	log.Print("Agent-C gateway ready on ", cfg.Listen)
	if err = srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Fatal("gateway listener failed")
	}
}
