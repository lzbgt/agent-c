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

## Design conclusion

Do not copy Hermes Python subsystem shapes directly into this C/C++/Go daemon-first repo. Preserve the local architecture:

- workflows are the execution substrate
- schedules are durable workflow submitters
- memory tools are explicit workflow tasks
- broker compatibility is adapter-based, not a fake claim of full protocol parity
- RL readiness starts with deterministic records and replay bundles before model-training loops
