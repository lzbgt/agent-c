package main

import (
	"context"
	"flag"
	"fmt"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"time"
)

func parseFlags() config {
	var cfg config
	flag.StringVar(&cfg.brokerBase, "broker-base", strings.TrimSpace(os.Getenv("BROKER_BASE")), "Broker base URL (env: BROKER_BASE).")
	flag.StringVar(&cfg.oidcToken, "oidc-token", strings.TrimSpace(os.Getenv("BROKER_OIDC_TOKEN")), "Bearer token for broker auth (env: BROKER_OIDC_TOKEN).")
	flag.StringVar(&cfg.oidcTokenFile, "oidc-token-file", strings.TrimSpace(os.Getenv("BROKER_OIDC_TOKEN_FILE")), "Path to broker auth token file (env: BROKER_OIDC_TOKEN_FILE).")
	flag.BoolVar(&cfg.insecureTLS, "insecure", strings.TrimSpace(os.Getenv("BROKER_INSECURE_TLS")) == "1", "Skip TLS verification (env: BROKER_INSECURE_TLS=1).")
	flag.DurationVar(&cfg.pollInterval, "poll-interval", 5*time.Second, "Polling interval when not --once.")
	flag.BoolVar(&cfg.once, "once", false, "Process one poll cycle and exit.")
	flag.IntVar(&cfg.limit, "limit", 50, "Max orchestrator runs to fetch per team per poll.")
	flag.StringVar(&cfg.status, "status", "running", "Orchestrator run status filter (default: running).")
	flag.BoolVar(&cfg.includePlanned, "include-planned", true, "Also include planned runs (auto-start).")
	flag.StringVar(&cfg.orchestratorID, "orchestrator-id", strings.TrimSpace(os.Getenv("ORCHESTRATOR_ID")), "Orchestrator loop id stored in run meta.")
	flag.BoolVar(&cfg.disableLedgerWatch, "disable-ledger-watch", false, "Use periodic reconciliation only, without requirement-change watches.")
	flag.IntVar(&cfg.ledgerWatchLimit, "ledger-watch-limit", 16, "Maximum concurrent authoritative requirement watches (1..64, or 0 for default 16); excess goals retain polling.")
	flag.Parse()
	if cfg.orchestratorID == "" {
		cfg.orchestratorID = "orchestrator_" + randID(8)
	}
	return cfg
}

func (c config) validate() error {
	if c.brokerBase == "" {
		return fmt.Errorf("missing broker base url")
	}
	if c.oidcToken == "" && c.oidcTokenFile == "" {
		return fmt.Errorf("missing oidc token")
	}
	if c.ledgerWatchLimit < 0 || c.ledgerWatchLimit > 64 {
		return fmt.Errorf("ledger-watch-limit must be 0 (default 16) or 1..64")
	}
	if c.limit <= 0 {
		c.limit = 50
	}
	if c.pollInterval <= 0 {
		c.pollInterval = 5 * time.Second
	}
	if c.status == "" {
		c.status = "running"
	}
	return nil
}

func (c config) bearerToken() (string, error) {
	if c.oidcTokenFile != "" {
		if data, err := os.ReadFile(c.oidcTokenFile); err == nil {
			token := strings.TrimSpace(string(data))
			if token != "" {
				return token, nil
			}
		} else if strings.TrimSpace(c.oidcToken) == "" {
			return "", fmt.Errorf("read oidc token file: %w", err)
		}
	}
	if strings.TrimSpace(c.oidcToken) == "" {
		return "", fmt.Errorf("missing oidc token")
	}
	return strings.TrimSpace(c.oidcToken), nil
}

func runLoop(ctx context.Context, client *http.Client, cfg config) {
	interval := cfg.pollInterval
	if interval <= 0 {
		interval = 5 * time.Second
	}
	var watches *ledgerWatchManager
	if !cfg.once && !cfg.disableLedgerWatch {
		watches = newLedgerWatchManager(ctx, client, cfg)
		defer watches.close()
	}
	rearm := true
	for {
		desired := map[string]ledgerWatchSpec{}
		var observe func(string, orchestratorRun)
		if watches != nil {
			observe = func(teamID string, run orchestratorRun) {
				if spec, ok := ledgerWatchForRun(run, cfg.orchestratorID); ok {
					desired[spec.key()] = spec
				}
			}
		}
		if err := runCycle(ctx, client, cfg, observe); err != nil {
			fmt.Fprintf(os.Stderr, "orchestrator error: %v\n", err)
		}
		if cfg.once {
			return
		}
		var wake <-chan struct{}
		if watches != nil {
			watches.sync(desired, rearm)
			wake = watches.wake
		}
		timer := time.NewTimer(interval)
		select {
		case <-ctx.Done():
			timer.Stop()
			return
		case <-wake:
			timer.Stop()
			rearm = false
		case <-timer.C:
			rearm = true
		}
	}
}

func runOnce(ctx context.Context, client *http.Client, cfg config) error {
	return runCycle(ctx, client, cfg, nil)
}
func runCycle(ctx context.Context, client *http.Client, cfg config, observe func(string, orchestratorRun)) error {
	teams, err := fetchTeams(ctx, client, cfg)
	if err != nil {
		return err
	}
	for _, team := range teams {
		teamID := strings.TrimSpace(team.TeamID)
		if teamID == "" {
			continue
		}
		runs := []orchestratorRun{}
		list, err := fetchOrchestratorRuns(ctx, client, cfg, teamID, cfg.status)
		if err != nil {
			return err
		}
		runs = append(runs, list...)
		if cfg.includePlanned && strings.ToLower(cfg.status) != "planned" {
			more, err := fetchOrchestratorRuns(ctx, client, cfg, teamID, "planned")
			if err != nil {
				return err
			}
			runs = append(runs, more...)
		}
		// Only coordinator-managed ledger waits wake automatically. Manual waits remain stopped.
		if cfg.status == "running" || cfg.status == "" {
			waiting, err := fetchOrchestratorRuns(ctx, client, cfg, teamID, "waiting")
			if err != nil {
				return err
			}
			for _, run := range waiting {
				if ledgerManagedWait(run) {
					runs = append(runs, run)
				}
			}
		}
		seen := map[string]bool{}
		for _, run := range runs {
			if seen[run.OrchestratorRunID] {
				continue
			}
			seen[run.OrchestratorRunID] = true
			if strings.TrimSpace(run.OrchestratorRunID) == "" {
				continue
			}
			if !shouldAutonomous(run.Meta) {
				continue
			}
			if err := handleRun(ctx, client, cfg, teamID, run); err != nil {
				fmt.Fprintf(os.Stderr, "orchestrator run %s error: %v\n", run.OrchestratorRunID, err)
			}
			if observe != nil {
				if _, bound := run.Meta["work_ledger"]; bound {
					// Re-arm from fresh authoritative goal metadata, never from a wake hint.
					if current, err := getLedgerGoal(ctx, client, cfg, teamID, run.OrchestratorRunID); err == nil {
						observe(teamID, current)
					}
				}
			}
		}
	}
	return nil
}

func buildRunContext() (context.Context, context.CancelFunc) {
	return signal.NotifyContext(context.Background(), os.Interrupt)
}
