# Agent-C

A portable agent core, durable process execution and a lean workspace for autonomous work.

Agent-C serves three connected uses:

| Use | Current foundation |
| --- | --- |
| **Embedded agent core** | Portable C sessions, bounded tool loops, streaming, compaction, completion/cancellation hooks and embedded wire/authentication helpers. Host services stay outside the core. |
| **Customizable process pipelines** | Versioned SOP stages/actions compile into a durable workflow DAG. Retries, evidence, scheduling and guarded HTTP effects support office, industrial and other defined processes. |
| **Autonomous agents** | Durable requirements, dependencies and scoped human-action blockers drive continued work. The CLI, daemon and optional broker share execution capabilities. |

Authorized work runs **autonomously by default**, without routine permission prompts. A genuine human dependency identifies the required action and affected requirements; unrelated work remains runnable. Explicit stops, resource limits, access controls and SOP business decisions remain effective.

This is a rolling project. We prefer coherent refactoring and fresh deployments over compatibility layers and migrations. [Current snapshot](docs/CURRENT_SNAPSHOT.md), [architecture](docs/ARCHITECTURE_CURRENT.md) and [primary-source research](docs/research/agent-systems-20261110.md) distinguish implemented behavior from the remaining work.

## A workspace built around the answer

Chat shows your messages and the agent's replies naturally, once, in chronological order. Files, notes and interactive results remain useful outputs. System context, tool records, timestamps, traces and execution settings are grouped under closed **Activity & details**.

Messages have one renderer: expanding Activity & details never repeats the chat. Tool pages lead with saved knowledge, summaries, playback controls and run outputs; technical evidence stays closed.

The composer focuses on **Attach** and **Send**. Settings leads with model, capabilities and conversation; technical controls and conversation management are collapsed by default. Desktop, phone and tablet layouts share these defaults. [UI design](docs/UI_DESIGN.md) records the attention hierarchy and verification contract.

Workflows has separate **Runs**, **Create workflow** and **Schedules** sections. Its SOP template defines ordered stages and sequential or parallel actions; JSON and graph authoring remain available. [SOP guide](docs/SOP_WORKFLOWS.md)

## What is implemented

- **Portable core:** C tool execution with bounded budgets and cancellation; hosts can continue a premature final response within the original limits.
- **Autonomous work state:** a SQLite requirement/blocker ledger shared by `work_state` and `agent work`, with dependency validation, revision checks and evidence-gated transitions.
- **Durable workflows:** SOP compilation, definition hashes, task recovery, scheduling, dataflow and inspectable evidence.
- **HTTP effects:** stable effect identity, intent/receipt journaling and reconciliation for guarded writes. Independent branches continue when another branch needs recovery. [Effect contract](docs/WORKFLOW_EFFECTS.md)
- **CLI harness:** `exec`, stdin, `resume`, `sessions`, explicit work/session directories, versioned JSONL events and successful final-message files. Coalesced streams drain without transport-boundary truncation. [CLI](docs/CLI.md)
- **Host platform:** native daemon APIs/SSE, memory, artifacts, policy/limits, diagnostic tools and optional broker routing, teams and event replay.
- **Authenticated deployment:** an Arch-hosted gateway serves the UI and protects the private daemon using shared Hubstack OAuth, server-held credentials and verified administrator identities.

These capabilities do not establish distributed exactly-once execution, arbitrary-tool effect safety, broker-authoritative requirements, complete SOP decisions/compensation or hardware qualification. Those boundaries and delivery priorities are explicit in the [snapshot](docs/CURRENT_SNAPSHOT.md) and [roadmap](TODOS.md).

## Build and run

Install the CMake/C/C++ and host dependencies listed in [Operations](docs/OPERATIONS.md), then build an assertion-enabled development snapshot:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DAGENT_BUILD_HOST=ON -DAGENTD_ENABLE_HTTP=ON
cmake --build build --target agent agentd -j 4
```

Configure provider credentials on the host using the [CLI guide](docs/CLI.md). Keep provider keys and service credentials out of browser configuration and committed files.

```sh
./build/agent exec "Inspect this project and implement the requested change"
printf '%s' 'Summarize this build result' | ./build/agent exec - --no-session
./build/agent exec --json -o final.txt "Verify the completed work"
./build/agent sessions
./build/agent resume <session-id>
./build/agent work
```

`--json` emits `agentd.cli.event.v1`; `-o` publishes the exact final text only after successful execution and session saving. [Machine-output protocol](docs/CLI_EVENT_PROTOCOL.md)

For WebUI development:

```sh
cd ui
npm ci
npm run dev
```

Connection and production build details are in [WebUI](docs/WEBUI.md). Tool extensions are documented in [Tools](docs/TOOLS.md).

## Reproduce the checked snapshot

The full verifier uses Debug native builds, local model stubs and mocked browser APIs. It does not need production provider keys:

```sh
cd ui
npm ci
npx playwright install chromium
cd ..
tools/verify_current_snapshot.sh
```

The current lane covers **20 native/CLI/API checks**, Go broker/gateway tests, **47 browser checks**, generated OpenAPI types and the production UI build. Browser cases include greeting clarity, streaming handoff, repeated identical turns, IME-safe sends, hidden metadata, subject-oriented settings, authorized browser effects and responsive workflow tools.

For the embedded-safe build:

```sh
tools/verify_current_snapshot.sh --core-only
```

Set `AGENT_E2E_BROWSER_EXECUTABLE` for an existing supported Chromium and `AGENT_E2E_UI_PORT` for an unused test port. Physical MCU and phone keyboard qualification remain separate from host/browser checks. Broader verification and Docker development lanes are documented in [Operations](docs/OPERATIONS.md).

## Deployment

The current reference deployment is [agent.hubstack.cn](https://agent.hubstack.cn), protected by shared Hubstack login. Execution, UI and the dedicated database run on Arch; the VPS supplies HTTPS reverse-proxy ingress and the existing identity service. UI-only releases preserve an unchanged running daemon.

[Arch deployment](docs/ARCH_HOST_DEPLOYMENT.md) documents private configuration, owner authorization, reproducible installation, immutable artifact manifests and live qualification. [Deployment](docs/DEPLOYMENT.md) describes other supported development/operations paths. The general Docker/Keycloak stack is a verification option, not the current production topology.

## Source map

| Area | Source / documentation |
| --- | --- |
| Portable C library | `core/` · [Embedded API](docs/EMBEDDED_C_API.md) |
| Native executor and CLI | `cli/` · [CLI](docs/CLI.md) · [Autonomous work](docs/AUTONOMOUS_WORK.md) |
| Daemon and workflow engine | `daemon/` · [Daemon](docs/AGENTD.md) · [SOPs](docs/SOP_WORKFLOWS.md) |
| Broker and authenticated gateway | `broker/` · [Arch deployment](docs/ARCH_HOST_DEPLOYMENT.md) |
| Responsive workspace | `ui/` · [WebUI](docs/WEBUI.md) · [UI design](docs/UI_DESIGN.md) |
| Reproducible checks and deployment | `tools/` · [Operations](docs/OPERATIONS.md) |

## Publishing

The owner's latest direction is to publish this verified snapshot to **GitHub**. Panda publishing remains on hold while that service is being fixed. Existing remotes are preserved; excluded Panda repositories are never recreated automatically. [Publishing policy](docs/PUBLISHING.md)

## Embedded example and support

The existing [ESP32 voice-agent demonstration](https://github.com/user-attachments/assets/be605653-04cf-49f1-9696-4dff41796635) illustrates an integration; it is not board-level qualification evidence for every current feature.

Optional contributions support public maintenance and experiments: [Support Agent-C](https://x2.brucelu.top/products/support/checkout/?source=github-agent-c-readme).
