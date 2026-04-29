# Hermes-Leverage Agentd Upgrade v0

Date: 2026-04-28
Status: implementation slice active

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

Implemented artifacts:

- `agentd_experience_record_v1` JSONL records at `state_dir/rl/experience_records.jsonl`
- `/api/v1/rl/experience_records` query/export endpoint with bounded paging and filters

Future work can add:

- rollup stats by label/reward/status
- conversion to Hermes-style trajectory JSONL
- eval-pack generation from successful and failed workflow records

## codexw broker/iOS compatibility

The sibling `codexw` broker contract is not the same as this repo's `agentd-broker` protocol.

Facts from `../codexw`:

- The iOS app talks to the `codexw` broker.
- `codexw deploy -u <user> -p <password>` can login to the broker, mint a one-time enrollment token, bootstrap a deployment certificate, and maintain an outbound broker connection.
- The broker-visible surface is deployment/session/shell/service/capability oriented.
- `codexw deploy` supports an external local API mode through `--deployment-local-api-base-url <url>` plus `--local-api-token <token>`.
- The broker live peer runtime polls and controls the external API using endpoints such as `/api/v1/runtime`, `/api/v1/session`, `/api/v1/session/{session_id}/turn/start`, `/api/v1/session/{session_id}/transcript`, and shell/file/capability routes.
- Compatibility target is partial protocol compatibility through explicit adapters, not full drop-in equivalence.

Therefore v0 uses a facade-backed bridge launcher, not a false native claim:

```bash
tools/run_agentd_codexw_compat.sh -u admin -p '<broker password>'
```

The default launcher mode:

- starts local `agentd` on loopback with an auth token
- starts `tools/agentd_codexw_local_api_facade.py` on loopback with a separate local API bearer token
- starts `codexw deploy` in this repo workspace with `--deployment-local-api-base-url` and `--local-api-token`
- keeps `AGENTD_BASE_URL`, `AGENTD_AUTH_TOKEN`, `CODEXW_LOCAL_API_BASE_URL`, and `CODEXW_LOCAL_API_TOKEN` exported for child process compatibility/debugging

The same launcher also supports native broker mode:

```bash
tools/run_agentd_codexw_compat.sh \
  --broker-mode native \
  --broker-url https://broker.example \
  --deployment-id agentd-m2 \
  -u admin \
  -p '<broker password>'
```

Native mode:

- starts local `agentd` on loopback with an auth token
- stores deployment key, CSR, broker-signed certificate, and enrollment
  material under `.codexw-agentd/native` unless `--native-identity-dir` is
  supplied
- bootstraps the deployment key/CSR/certificate on first run when enrollment
  token credentials are supplied, or logs in with `-u/-p` and self-issues a
  one-time enrollment token before certificate enrollment
- reuses the persisted deployment identity on later runs without needing the
  one-time enrollment token again
- starts `tools/agentd_codexw_native_broker_connector.py --connect`
  instead of `codexw deploy`
- sends the same runtime actions and capability manifest as the facade bridge,
  but over codexw's native deployment websocket

Facade route contract:

- `GET /healthz`
- `GET /api/v1/runtime`
- `POST /api/v1/runtime/actions`
- `GET /api/v1/session`
- `POST /api/v1/session/new`
- `POST /api/v1/session/attach`
- `POST /api/v1/session/{session_id}/turn/start`
- `GET /api/v1/session/{session_id}/transcript`
- `GET /api/v1/session/{session_id}/orchestration/status`
- `GET /api/v1/session/{session_id}/orchestration/dependencies`
- `GET /api/v1/session/{session_id}/orchestration/workers`
- `GET /api/v1/session/{session_id}/collaboration/capability-report`
- `GET /api/v1/session/{session_id}/capabilities`
- `GET /api/v1/session/{session_id}/services`
- `GET /api/v1/session/{session_id}/shells`
- `GET /api/v1/session/{session_id}/files/read`

In production mode the facade forwards mobile `turn/start` prompts to `agentd /api/v1/run`. In smoke/offline mode it can run with `--turn-mode echo` to verify the broker-facing contract without a model provider key.

`tools/agentd_codexw_contract.py` is the shared source for the agentd runtime
contract used by both the facade bridge and the native broker connector work.
It owns the `broker.runtime_capabilities.v1` action manifest, legacy capability
strings, canonical JSON capability hashing, runtime identity headers, and the
minimal `deployment.snapshot` frame shape. Keeping this in one module prevents
facade-backed and native broker paths from drifting.

`/api/v1/runtime` advertises a `broker.runtime_capabilities.v1` manifest so the
codexw broker and app can drive agentd-specific panels through capabilities
instead of hardcoded runtime branches. The generic
`POST /api/v1/runtime/actions` adapter supports:

- `workflow.submit` -> `POST /api/v1/workflow/submit`
- `workflow.read` -> `GET /api/v1/workflow?workflow_id=...`
- `workflow.cancel` -> `POST /api/v1/workflow/cancel`
- `schedule.list` -> `GET /api/v1/workflow_schedules`
- `experience.list` -> `GET /api/v1/rl/experience_records`
- `experience.export` -> `GET /api/v1/rl/experience_records` with an
  explicit export-format marker in the facade result

Known bridge limits:

- Shell control is represented but not yet backed by live `agentd` shell ownership.
- The transcript is facade-local for now; a later slice should mirror `agentd` session/audit data into the codexw transcript page model.
- Native `agentd` direct enrollment into the `codexw` broker now has a local
  connector with a websocket loop, runtime snapshots, and
  `deployment.command` results. It also has persisted identity bootstrapping
  and a `tools/run_agentd_codexw_compat.sh
  --broker-mode native` launcher path. The repo-local E2E smoke now exercises
  this native launcher through a real sibling `codexw` broker process, including
  login, user-owned enrollment token issuance, certificate enrollment, websocket
  deployment connect, runtime inventory projection, capability discovery, and a
  broker runtime action round trip. Production deployment should run `agentd`
  and the connector as separate foreground services under launchd, systemd,
  Docker Compose, Kubernetes, or an equivalent process manager; connector
  reconnect supervision is for manual foreground debugging only.
- Codexw cloud/app should grow first-class `agentd` capability models for workflow schedules, closed-loop experience records, RL export, and delegate/parallel status instead of treating them as opaque runtime metadata.

## Native codexw broker enrollment target

The sibling `codexw` broker now accepts native non-codexw runtimes through the
same broker-signed deployment certificate websocket path used by `codexw`
deployments. Native `agentd` should keep the existing facade bridge until it
implements this full deployment protocol, but the target handshake is now
concrete.

`tools/agentd_codexw_native_broker_connector.py` implements the first native
agentd broker-client:

- computes the same canonical `broker.runtime_capabilities.v1` SHA-256 that
  the broker stores as runtime identity metadata
- builds signed deployment-connect headers for
  `GET /api/v1/deployment/connect`
- emits native runtime identity headers for `runtime_kind:"agentd"`
- builds the runtime payload expected inside a `deployment.snapshot` frame
- opens the broker deployment websocket with a stdlib RFC 6455 client
- sends `deployment.hello` and `deployment.snapshot` frames
- handles broker ping frames and periodic snapshot refresh
- dispatches `deployment.command` for `GET /api/v1/runtime`,
  `GET /healthz`, and `POST /api/v1/runtime/actions`
- can sign and submit a CSR body to
  `POST /api/v1/deployment/enroll-certificate` when supplied an enrollment
  token id, shared secret, and CSR PEM
- can login to `POST /api/v1/auth/login` with broker `-u/-p`, issue a
  user-owned token through `POST /api/v1/auth/deployment-enrollment-tokens`,
  and use that token for first-boot certificate enrollment
- can bootstrap a persistent identity directory with deployment key, CSR,
  broker-signed certificate, and raw enrollment response material
- supports optional reconnect supervision with bounded exponential backoff for
  manual foreground debugging; production units should omit `--reconnect` and
  let the platform process manager own restart/backoff
- provides `--dry-run` so tests and operators can inspect the exact broker
  contract without needing a live broker or websocket dependency

Example dry run:

```bash
tools/agentd_codexw_native_broker_connector.py \
  --broker-url https://broker.example \
  --deployment-id agentd-m2 \
  --deployment-cert-path state/codexw/deployment.cert.pem \
  --deployment-key-path state/codexw/deployment.key.pem \
  --agentd-base-url http://127.0.0.1:18080 \
  --dry-run
```

Example websocket connect:

```bash
tools/agentd_codexw_native_broker_connector.py \
  --broker-url https://broker.example \
  --deployment-id agentd-m2 \
  --deployment-cert-path state/codexw/deployment.cert.pem \
  --deployment-key-path state/codexw/deployment.key.pem \
  --agentd-base-url http://127.0.0.1:18080 \
  --connect
```

Native `agentd` should:

1. Login to the broker as a user or receive a one-time deployment enrollment
   token.
2. Generate and persist a deployment private key locally.
3. Submit a CSR to `POST /api/v1/deployment/enroll-certificate` using the
   one-time enrollment token.
4. Connect outbound to `GET /api/v1/deployment/connect` with the broker-signed
   deployment certificate headers and the deployment-certificate request
   signature used by `codexw`.
5. Include these runtime identity headers during connect:
   - `X-Codexw-Deployment-Mode: service`
   - `X-Codexw-Runtime-Kind: agentd`
   - `X-Codexw-Runtime-Instance-Id: <stable agentd runtime instance id>`
   - `X-Codexw-Runtime-Capabilities-SHA256: <sha256 of canonical broker.runtime_capabilities.v1>`
   - `X-Codexw-Runtime-Host-Id: <logical host id>`
   - `X-Codexw-Runtime-Target-OS: <os>`
   - `X-Codexw-Runtime-Target-Arch: <arch>`
6. Send normal `deployment.snapshot` websocket frames whose runtime payload
   matches the same `runtime_kind`, `instance_id`, host, OS, arch, and
   capability manifest.
7. Implement `deployment.command` dispatch for the local API paths already
   exposed by the facade, especially `POST /api/v1/runtime/actions`.

The broker treats the header identity as authenticated deployment metadata once
the certificate is verified and the deployment is approved. It uses the metadata
to populate pending/approved enrollment records and `/api/v2/runtime-instances`
before the first snapshot arrives. Snapshot data remains the richer source for
full capability manifests and runtime details.

## Service app deployment model

The native codexw connector is a service app in the Unix sense: it is an
ordinary foreground process with one protocol responsibility. It must not become
a second daemon manager around `agentd`, and it should not be folded into the C++
daemon just to own lifetime. The production composition is:

1. `agentd` runs as its own OS-managed service.
2. `tools/agentd_codexw_native_broker_connector.py --connect` runs as its own
   OS-managed service.
3. The platform manager owns restart/backoff, boot ordering, logs, and
   observability.

The repo now provides:

- `packaging/systemd/agentd.service`
- `packaging/systemd/agentd-codexw-connector.service`
- `tools/install_agentd_launchd.sh`
- `tools/install_agentd_codexw_connector_launchd.sh`

Both connector service paths intentionally omit `--reconnect`; they rely on the
platform restart policy instead.

## Acceptance for this slice

- CMake builds with the new `experience_record` module.
- `agentd_workflow_experience_record_smoke` passes.
- `agentd_codexw_local_api_facade_smoke` passes.
- `agentd_codexw_native_broker_connector_smoke` passes.
- `agentd_codexw_native_broker_e2e_smoke` passes when the sibling
  `~/work/codexw/broker` repo and Go toolchain are available. It starts a real
  codexw broker fixture and verifies native `agentd` runtime action routing
  through `/api/v2/runtime-instances/{instance_id}/actions`.
- `agentd_codexw_service_installers_smoke` passes and verifies launchd/systemd
  service artifacts keep process lifetime in the platform manager.
- `tools/run_agentd_codexw_compat.sh` passes shell syntax validation.
- `tools/run_agentd_codexw_compat.sh --broker-mode native` has a documented
  first-boot and steady-state identity path.
- Research and design are stored as Markdown in the repo for reuse.
