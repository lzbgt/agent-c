package main

import (
	"agentd-broker/internal/localui"
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
	var cfg localui.Config
	flag.StringVar(&cfg.Listen, "listen", "127.0.0.1:18088", "Loopback IP and port")
	flag.StringVar(&cfg.DaemonURL, "daemon-url", "http://127.0.0.1:18090", "Local agentd URL")
	flag.StringVar(&cfg.DaemonTokenFile, "daemon-token-file", "", "Private agentd bearer token file")
	flag.StringVar(&cfg.LoginKeyFile, "login-key-file", "", "Private browser login key file")
	flag.StringVar(&cfg.WebDir, "web-dir", "", "Absolute path to built Agent C ui/dist")
	flag.StringVar(&cfg.Model, "model", "deepseek-qwen35-heretic-4b", "Pinned local model")
	flag.StringVar(&cfg.ModelBaseURL, "model-base-url", "http://127.0.0.1:18089/v1", "Pinned loopback model API")
	flag.IntVar(&cfg.MaxOutputTokens, "max-output-tokens", 8192, "Per-response output token budget")
	flag.IntVar(&cfg.MaxChars, "max-chars", 480000, "Agent compaction character threshold")
	flag.IntVar(&cfg.TimeoutMS, "timeout-ms", 600000, "Model request timeout")
	flag.IntVar(&cfg.MaxSteps, "max-steps", 32, "Maximum model/tool-loop steps")
	flag.IntVar(&cfg.MaxToolCallsTotal, "max-tool-calls-total", 64, "Maximum tool calls per run")
	flag.IntVar(&cfg.MaxRepeatedToolCalls, "max-repeated-tool-calls", 3, "Maximum repeated identical tool calls")
	flag.IntVar(&cfg.MaxToolCallArgsChars, "max-tool-call-args-chars", 65536, "Maximum tool argument characters")
	flag.IntVar(&cfg.MaxToolResultChars, "max-tool-result-chars", 24000, "Maximum tool result characters in model context")
	flag.Parse()
	app, err := localui.New(cfg)
	if err != nil {
		log.Fatal(err)
	}
	srv := &http.Server{Addr: cfg.Listen, Handler: app, ReadHeaderTimeout: 5 * time.Second, ReadTimeout: 30 * time.Second, IdleTimeout: 60 * time.Second, MaxHeaderBytes: 16 << 10}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	go func() {
		<-ctx.Done()
		deadline, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = srv.Shutdown(deadline)
	}()
	log.Printf("Local agent web interface listening on http://%s", cfg.Listen)
	if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Fatal(err)
	}
}
