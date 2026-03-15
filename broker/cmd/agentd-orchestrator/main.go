package main

import (
	"fmt"
	"os"
)

func main() {
	cfg := parseFlags()
	if err := cfg.validate(); err != nil {
		fmt.Fprintf(os.Stderr, "config error: %v\n", err)
		os.Exit(2)
	}
	ctx, stop := buildRunContext()
	defer stop()

	client := httpClient(cfg.insecureTLS)
	runLoop(ctx, client, cfg)
}
