Status (as of 2026-02-03)

- [x] 1) Broker mode is first-class in WebUI (broker proxy + SSE, agent picker).
- [x] 2) Provider reliability layer (retries/backoff/timeouts + smoke tests).
- [x] 3) Job lifecycle durability (DB-backed job rows + restart marks inflight as `interrupted`).
- [x] 4) Structured durable memory + deterministic conflict handling + retrieval policy knobs.
- [x] 5) Multi-agent orchestration:
  - daemon: `POST /api/v1/orchestrate` (fan-out + optional session writeback)
  - broker: `POST /v1/orchestrate` (fan-out across multiple agents via broker relay)

1. Make Broker Mode a first-class runtime (end-to-end usability from anywhere)

  - Why highest leverage: you already built the hardest parts (broker + connector + agentd API shape). Wiring this into the WebUI makes the system
    “global-by-default” instead of “localhost-by-default”.
  - Concrete deliverables:
      - WebUI connection mode: direct agentd vs via broker (agent_id) with two auth types (agentd token vs OIDC token).
      - Teach WebUI to route normal HTTP endpoints via broker proxy and SSE via broker’s SSE proxy (broker explicitly supports this in docs/
        BROKER.md:1).
      - Add minimal UX: agent picker (GET /v1/agents), connectivity indicator (connected/last_seen), and “copy shareable agent link”.
  - Files/modules this would touch (starting points): ui/src/App.tsx:1 (currently assumes direct daemon base), docs/BROKER.md:1 (proxy shapes), docs/
    PROTOCOL.md:1 (what UI calls).
  - Proof: tools/verify_compose_stack.sh:1 becomes a true E2E demo where the WebUI can drive agentd through the broker (not just curl can).

  2. Provider reliability layer (turn prototype into daily-driver)

  - Why: right now the system is architecturally strong, but “real world” agenting lives/dies on backoff/jitter/timeouts, and consistent error
    classification—your own backlog calls this out as top production readiness work (TODOS.md:1).
  - Concrete deliverables:
      - Unified retry policy for LLM calls (429/5xx/timeouts) with capped exponential backoff + jitter; never retry tool execution.
      - Separate connect timeout / total timeout / streaming idle timeout.
      - Emit structured “retry” events so UI + DB can explain what happened (ties into docs/DB.md:1).
  - Proof: add stub-server tests that force 429→200 and 500→200 flows (there’s already a strong testing culture under tests/:1).

  3. Job lifecycle durability (make async runs resilient to UI refresh + daemon restarts)

  - Why: you already have async jobs + SSE + DB; the “missing piece” is making job state unambiguous and inspectable after failure/restart
    (explicitly listed in TODOS.md:1).
  - Concrete deliverables:
      - Persist minimal job metadata in SQLite (status transitions + last heartbeat + stop reason).
      - UI shows: “job still running but connection dropped” vs “job failed” (your UI already has the right conceptual split: see the jobNotice vs
        jobError intent in ui/src/App.tsx:1).
  - Proof: a test that starts async job, restarts daemon mid-run, and confirms UI can at least show a truthful terminal state (“interrupted by
    restart”, not “timed out”).

  4. Upgrade “memory” from a log into an agentic subsystem (retrieval + conflict management)

  - Why: memory exists (tools + injection + “memory flush” in core), but today it’s effectively Markdown append/search (see cli/src/
    toolset_host_memory.cpp:1, and how memory is injected in daemon/src/run_endpoints.cpp:1 via references in the ripgrep output you saw). The next
    leap is: “reliable retrieval + deconfliction + scope”.
  - Concrete deliverables:
      - Define a small schema for durable facts/preferences/tasks (even if stored as Markdown + frontmatter).
      - Add deterministic conflict resolution rules (newer overrides older; explicit “deprecated” markers) and enforce via memory_put.
      - Add “session vs core vs daily” retrieval policy knobs in run requests.
  - Proof: unit tests that show stale facts get replaced, not accumulated.

  5. Unlock “agentic versatility” via multi-agent orchestration (local + broker)

  - Why: once broker mode is real, the system can coordinate multiple connected agents (workstation agent, GPU agent, device agent). That’s the
    difference between “an agent” and “an agentic system”.
  - Concrete deliverables:
      - A daemon-level “orchestrator run” mode: fan-out subtasks to multiple agents, gather results, and write a combined answer back into the
        originating session.
      - Explicit budgets + tool policies per sub-agent (ties directly into your limits model in docs/LIMITS.md:1).
