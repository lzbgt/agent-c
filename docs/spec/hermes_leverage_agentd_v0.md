# Hermes-Leverage Agentd Upgrade v0

Date: 2026-04-28
Status: implementation slice started

## Purpose

Use the concrete feature set in Nous Research's Hermes Agent as leverage without changing this repo's architecture.

The goal is to strengthen this `agent` project in four areas:

1. closed-loop learning
2. scheduled tasks
3. delegation and parallel execution
4. RL-ready data infrastructure

A second product goal is compatibility with the sibling `../codexw` broker path so this daemon workspace can be reachable from the iOS app through the broker-backed deployment model.

## Source facts

Reference material is stored locally:

- Hermes full checkout: `ref/third_party_fetch/hermes-agent`
- Hermes leverage note: `docs/research/hermes-agent-leverage-20260428.md`
- codexw broker contract docs:
  - `../codexw/docs/codexw-broker-adapter-contract.md`
  - `../codexw/docs/codexw-broker-connectivity.md`
  - `../codexw/docs/codexw-embedded-broker-connect-plan.md`
  - `../codexw/broker/README.md`

## Existing local foundation

This repo already ships the pieces Hermes uses as top-level product features:

- Durable workflows with retries, priorities, deadlines, fair scheduling, expectations, and replay bundles.
- Workflow schedules with cron-backed submit semantics.
- Delegation and fanout through `delegate`, `delegate_parallel`, `agentd_parallel`, aggregates, quorum checks, and broker registry target expansion.
- Durable memory through structured checkpoints, consolidation, salience, recaps, timeline/query/correlation surfaces, and memory tasks.

The missing primitive was a compact record of "what was attempted, what happened, and what reward/evaluation signal should be retained" that can be consumed by later learning, audits, and RL pipelines.

## Slice 1: experience records

Add a workflow task kind:

```json
{
  "task_id": "learn",
  "kind": "experience_record",
  "depends_on": ["solve"],
  "experience_record": {
    "label": "nightly/audit",
    "task_ids": ["solve"],
    "reward": 1.0,
    "metadata": { "source": "schedule" }
  }
}
```

Semantics:

- Reads completed dependency/source task results from the workflow result map.
- Writes one JSONL record to `state_dir/rl/experience_records.jsonl`.
- Emits a workflow event of type `experience_record`.
- Stores bounded source result surfaces, not unbounded transcripts.
- Accepts label-safe labels up to 128 chars; `/` is allowed for namespacing.
- Uses reward in `[-1, 1]`; if omitted, reward is `ok_count / source_task_count`.

Record schema:

```json
{
  "schema": "agentd_experience_record_v1",
  "workflow_id": "wf_...",
  "task_id": "learn",
  "session_id": "",
  "trace_id": "trace...",
  "ts_unix_ms": 1760000000000,
  "label": "nightly/audit",
  "reward": 1.0,
  "source_task_count": 1,
  "source_task_ok_count": 1,
  "source_results_by_task": {
    "solve": {
      "ok": true,
      "kind": "delay",
      "result": {}
    }
  },
  "metadata": {}
}
```

This is deliberately deterministic and file-based first. A later DB mirror can index these records, but JSONL is already usable by external trainers, eval-pack builders, and offline analysis.

## Closed-loop learning pattern

Recommended durable workflow pattern:

1. Execute a solve/audit/research task.
2. Use `expect` to make correctness checks deterministic.
3. Add `experience_record` to persist outcome and reward.
4. Add `memory_put` for human-readable lessons worth reusing.
5. Run `memory_consolidate` as a scheduled maintenance job.

This keeps learning explicit:

- structured experience goes to RL/eval data
- durable memory receives distilled, operator-readable facts
- scheduled jobs decide when to consolidate
- no hidden model self-editing is needed for v0

## Scheduled tasks

Do not add a second scheduler. The existing workflow schedule engine is the substrate.

High-leverage scheduled jobs:

- nightly memory consolidation
- nightly or per-project audit workflows
- weekly experience export/rollup
- dependency/license/security scans with `experience_record`
- broker deployment health checks once `codexw` compatibility is active

## Delegation and parallel

Do not duplicate `delegate_parallel` or `agentd_parallel`.

Next useful upgrades:

- expose `experience_record` in delegate/parallel examples so every fanout can persist an evaluation row
- add UI affordances around delegate attempt summaries and reward labels
- add schedule templates for quorum and best-of-N workflows
- keep broker-target expansion as the multi-deployment mechanism

## RL-ready infrastructure

RL readiness in this repo should mean:

- deterministic replay bundles for runs
- deterministic experience JSONL for workflow outcomes
- bounded result payloads
- explicit reward and metadata
- stable schema names
- offline export paths that do not require a live provider key

The first implemented artifact is `agentd_experience_record_v1`. Future work can add:

- `/api/v1/rl/experience_records` query/export endpoint
- rollup stats by label/reward/status
- conversion to Hermes-style trajectory JSONL
- eval-pack generation from successful and failed workflow records

## codexw broker/iOS compatibility

The sibling `codexw` broker contract is not the same as this repo's `agentd-broker` protocol.

Facts from `../codexw`:

- The iOS app talks to the `codexw` broker.
- `codexw deploy -u <user> -p <password>` can login to the broker, mint a one-time enrollment token, bootstrap a deployment certificate, and maintain an outbound broker connection.
- The broker-visible surface is deployment/session/shell/service/capability oriented.
- Compatibility target is partial protocol compatibility through explicit adapters, not full drop-in equivalence.

Therefore v0 uses a bridge launcher, not a false native claim:

```bash
tools/run_agentd_codexw_compat.sh -u admin -p '<broker password>'
```

The launcher:

- starts local `agentd` on loopback with an auth token
- exports `AGENTD_BASE_URL` and `AGENTD_AUTH_TOKEN`
- starts `codexw deploy` in this repo workspace so the iOS app can reach a broker-connected deployment

Native `agentd` direct enrollment into the `codexw` broker remains a later phase because it requires implementing the `codexw` deployment certificate, signed request, websocket frame, deployment snapshot, command dispatch, and approval model directly.

## Acceptance for this slice

- CMake builds with the new `experience_record` module.
- `agentd_workflow_experience_record_smoke` passes.
- `tools/run_agentd_codexw_compat.sh` passes shell syntax validation.
- Research and design are stored as Markdown in the repo for reuse.
