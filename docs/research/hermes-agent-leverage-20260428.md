# Hermes Agent leverage notes

Date: 2026-04-28

Reference checkout:

- Source: `https://github.com/NousResearch/hermes-agent`
- Local copy: `ref/third_party_fetch/hermes-agent`
- Commit inspected: `474c725b49b8a6d343e3527ffe7e0fb3bb3c5dbf`

## Relevant Hermes facts

Hermes positions four capability groups that map directly to this repo's current roadmap:

- closed learning loop: agent-curated memory, self-created skills, self-improving skills, session search, user modeling
- scheduled automations: cron scheduler with unattended natural-language jobs and delivery to messaging platforms
- delegation and parallelism: isolated subagents, parallel workstreams, and RPC-based tool calls from scripts
- RL readiness: batch trajectory generation, Atropos environments, and trajectory compression for training/evaluation data

Files that anchor those claims in the checked-out repo:

- `ref/third_party_fetch/hermes-agent/README.md`
- `ref/third_party_fetch/hermes-agent/agent/memory_manager.py`
- `ref/third_party_fetch/hermes-agent/agent/trajectory.py`
- `ref/third_party_fetch/hermes-agent/cron/jobs.py`
- `ref/third_party_fetch/hermes-agent/cron/scheduler.py`
- `ref/third_party_fetch/hermes-agent/environments/`
- `ref/third_party_fetch/hermes-agent/batch_runner.py`
- `ref/third_party_fetch/hermes-agent/mini_swe_runner.py`

## Current local project match

The local `agent` repo already has strong equivalents for three of the four areas:

- scheduled tasks: durable workflow schedules are implemented and documented in `docs/WORKFLOWS.md`, with endpoints under `/api/v1/workflow_schedules`
- delegation and parallelism: workflow `delegate`, `delegate_parallel`, `agentd_parallel`, aggregate tasks, quorum modes, and broker registry target expansion are implemented
- memory foundation: `memory_put`, `memory_consolidate`, `memory_query`, `memory_correlate`, checkpoints, salience, recaps, and retention are implemented

The main gap versus Hermes was not another scheduler or parallel task primitive. The higher-leverage gap was a stable, deterministic "experience" artifact that can connect workflow execution, memory consolidation, scheduled audits, and future RL/evaluation pipelines.

## Work performed from this research

- Added workflow `kind:"experience_record"` to persist RL-ready JSONL records under `state_dir/rl/experience_records.jsonl`.
- Added smoke coverage proving a workflow can run, write an experience record, expose reward metadata, and leave a durable JSONL artifact.
- Drafted `docs/spec/hermes_leverage_agentd_v0.md` to define the closed-loop learning, schedule, delegation, RL, and `codexw` broker compatibility direction.
- Added `tools/run_agentd_codexw_compat.sh` as an operator bridge for the current sibling `codexw` broker/iOS deployment path.
- Added `tools/agentd_codexw_local_api_facade.py` so the bridge uses codexw's documented external-local-API deployment mode instead of only colocating `agentd` beside a broker-connected `codexw` process.
- Extended the shared codexw runtime-instance contract to advertise and prove
  `session_create` and bounded `proxy_http` for the native deployment connector,
  plus `proxy_sse` for the external local-API facade where codexw has a real
  streamable local API URL.
- Local fixture and broker E2E proofs now cover the expanded codexw contract:
  native websocket connector session creation, bounded runtime HTTP proxy, and
  facade-only SSE proxy. A strict live proof against the default public broker
  on 2026-05-04 reached the deployed broker but failed with `405
  method_not_allowed` on `POST /api/v2/runtime-instances/{id}/sessions`, which
  identifies a stale codexw broker deployment that must be rolled forward before
  the expanded live proof can pass.
- The public codexw broker was rolled forward the same day through codexw's
  audited `scripts/broker-admin broker-update` route to build revision
  `9dda0eeb7c16fb26a1e403de52e59b50d42c51e6`. The strict live activity proof
  then passed through
  `scripts/verify-codexw-release-contracts --agent-root /Users/zongbaolu/work/agent --agentd-run-live --agentd-proof activity --skip-doc-consistency --skip-runtime-proxy-fixture`.
- The remaining strict live proof set also passed after the broker rollout:
  `connector-readiness`, `ota-candidate`, and `restart` were run with
  `--agentd-run-live --keep-going` through the codexw release-contract verifier.
  This covers connector readiness/audit transitions, OTA candidate preflight,
  and restart preflight/dispatch/idempotency against the deployed public broker.
- The durable macOS connector-service proof was then closed on this host.
  `com.agentd.daemon`, `com.agentd.codexw-connector`, and
  `com.agentd.codexw-connector.self-test` were installed as LaunchAgents for a
  separate service deployment id, `agentd-service-bruce-mac`. The connector was
  bootstrapped once to persist its broker-signed deployment identity, then
  reinstalled with `AGENTD_CODEXW_BROKER_TOKEN` only so the final plist no
  longer carries broker username/password. `tools/verify_agentd_codexw_connector_service.py
  --json` passed `identity_files`, `identity_certificate_fingerprint`,
  `agentd_health`, `runtime_sessions_surface`, `runtime_events_surface`, and
  `broker_runtime_instance_visible`.
- The complete strict codexw verifier now passes with both service and live
  proof modes enabled:
  `scripts/verify-codexw-release-contracts --agent-root /Users/zongbaolu/work/agent --agentd-run-service --agentd-run-live --skip-doc-consistency --skip-runtime-proxy-fixture --keep-going`.
  The 2026-05-04 result logged under
  `/tmp/codexw-logs/codexw-release-contracts-20260504-110401` reports five
  passed proofs: `connector-service`, `connector-readiness`, `activity`,
  `ota-candidate`, and `restart`.

## Design conclusion

Do not copy Hermes Python subsystem shapes directly into this C/C++/Go daemon-first repo. Preserve the local architecture:

- workflows are the execution substrate
- schedules are durable workflow submitters
- memory tools are explicit workflow tasks
- broker compatibility is adapter-based, not a fake claim of full protocol parity
- codexw cloud/app still needs first-class surfaces for `agentd` features that are richer than the current codexw session/shell model: workflow schedules, experience records, RL export, and delegate/parallel topology
- RL readiness starts with deterministic records and replay bundles before model-training loops
