# agent vs codexw

Date: 2026-04-08
Author: Codex
Scope: compare `/Users/zongbaolu/work/agent` and `/Users/zongbaolu/work/codexw`, then decide whether both repos are justified.

## Executive conclusion

You do not need two repos if your product goal is singular.

You do need two repos if you intentionally want to own both:

- a first-party agent platform with its own runtime, daemon, broker, WebUI, and multi-provider model support
- a Codex-specific wrapper/deployment stack that stays aligned to the upstream `codex` app-server and also owns iPhone and brokered remote-control flows

Today the two repos are not duplicates. They are different products with some control-plane overlap.

The highest-risk duplication is not the runtime itself. It is the broker/control-plane lane:

- both repos have their own Go broker
- both repos talk about remote session control, event streaming, shell/service inspection, and deployment routing

So the practical answer is:

- keep both repos only if you want to support both runtime families
- otherwise collapse onto one repo and demote the other to reference/integration status

## Evidence

### 1. The repos state different product identities

`agent` describes itself as a full platform:

- production-oriented agentic platform with portable core, daemon, broker, CLI, and WebUI
- own runtime capabilities: workflows, team orchestration, memory tooling, policy hooks, OTA updates

Sources:

- `/Users/zongbaolu/work/agent/README.md:3`
- `/Users/zongbaolu/work/agent/README.md:12`
- `/Users/zongbaolu/work/agent/README.md:19`

`agent` project intent is also explicitly custom-runtime-oriented:

- multi-provider support
- daemon mode
- secure cloud broker exposure
- C++ implementation aimed at efficiency and portability, including embedded targets

Sources:

- `/Users/zongbaolu/work/agent/project.md:3`
- `/Users/zongbaolu/work/agent/project.md:9`
- `/Users/zongbaolu/work/agent/project.md:11`

`codexw` describes itself as an inline client for the upstream Codex runtime:

- inline terminal client for the official `codex app-server`
- does not patch Codex
- uses the Homebrew-installed vanilla `codex` binary as backend

Sources:

- `/Users/zongbaolu/work/codexw/README.md:3`
- `/Users/zongbaolu/work/codexw/README.md:13`

`codexw` also owns deployment/mobile-specific runtime concerns:

- broker-facing local API
- deployment bootstrap and reconnect
- iPhone app and remote client path

Sources:

- `/Users/zongbaolu/work/codexw/README.md:42`
- `/Users/zongbaolu/work/codexw/README.md:48`
- `/Users/zongbaolu/work/codexw/README.md:53`
- `/Users/zongbaolu/work/codexw/README.md:59`

### 2. The codebases are materially different

Working-tree / tracked-file snapshot taken on 2026-04-08:

`agent`

- tracked files: 2143
- working-tree files from `rg --files`: 2244
- dominant file types: `ts`, `sh`, `cpp`, `md`, `h`, `tsx`, `go`
- top directories by size: `state`, `ref`, `refs`, `docs`, `ui`, `daemon`, `core`, `tests`

`codexw`

- tracked files: 919
- working-tree files from `rg --files`: 951
- dominant file types: `rs`, `md`, `go`, `swift`
- top directories by size: `wrapper`, `artifacts`, `ios`, `build`, `ref`, `broker`

Interpretation:

- `agent` is a mixed-language platform repo centered on C++ + TS/React + Go
- `codexw` is primarily a Rust wrapper/runtime repo with an iOS app and a separate Go broker

The large `codexw` disk size is mostly working-tree artifacts rather than proof that the source tree is larger than `agent`. The meaningful comparison is tracked files and language mix.

### 3. The brokers are separate implementations, not shared code

Broker directory comparison:

- `agent/broker`: 105 files
- `codexw/broker`: 53 files

Common relative paths in broker trees:

- `README.md`
- `go.mod`
- `go.sum`

That means there is effectively no shared broker source layout across the two repos.

Functional split from repo docs:

`agent/broker`

- `agentd` relay / control plane
- OIDC-authenticated HTTP endpoints
- proxy/orchestrate/events model for connected `agentd` instances

Sources:

- `/Users/zongbaolu/work/agent/broker/README.md:3`
- `/Users/zongbaolu/work/agent/broker/README.md:7`
- `/Users/zongbaolu/work/agent/broker/README.md:22`

`codexw/broker`

- broker for authenticated outbound deployment connections from private hosts
- deployment summaries, runtime routing, user sessions, and mobile-facing flows
- SQLite + Pebble storage model

Sources:

- `/Users/zongbaolu/work/codexw/broker/README.md:3`
- `/Users/zongbaolu/work/codexw/broker/README.md:18`
- `/Users/zongbaolu/work/codexw/broker/README.md:23`

### 4. The repos are intentionally coupled as siblings, not accidentally duplicated

The `agent` repo explicitly treats `codexw` as an upstream surface to consume:

- "The goal is not to redesign `codexw`"
- "`agent` should consume the `codexw` broker-facing surface that already exists today"

Sources:

- `/Users/zongbaolu/work/agent/docs/spec/codexw_broker_webui_handoff_v0.md:12`
- `/Users/zongbaolu/work/agent/docs/spec/codexw_broker_webui_handoff_v0.md:18`
- `/Users/zongbaolu/work/agent/docs/spec/codexw_broker_webui_handoff_v0.md:40`

The `codexw` repo explicitly names `~/work/agent` as a consumer:

- implementation-facing handoff for the sibling `~/work/agent` workspace
- tells `~/work/agent` what it can rely on from the current broker-facing adapter

Sources:

- `/Users/zongbaolu/work/codexw/docs/codexw-broker-integration-handoff.md:5`
- `/Users/zongbaolu/work/codexw/docs/codexw-broker-integration-handoff.md:19`
- `/Users/zongbaolu/work/codexw/docs/codexw-broker-integration-handoff.md:49`

There is even explicit `codexw` coverage in `agent` WebUI tests:

- broker artifact boundary
- broker session events resume
- broker session lease
- broker session operators
- cross-deployment handoff

Source:

- `/Users/zongbaolu/work/agent/ui/package.json:14`

Interpretation:

- this is not "two people built the same thing twice"
- this is "one repo is already trying to consume the other repo as a remote runtime surface"

## Decision

### If the goal is one product only

You should not keep both repos as peer products.

Choose one:

- Choose `agent` if the core goal is your own runtime/platform: multi-provider, custom daemon, custom broker, WebUI, workflows, memory, embedded portability.
- Choose `codexw` if the core goal is operationalizing the official Codex runtime: upstream-compatible terminal UX, local API adapter, deployment bootstrap, runtime handoff/upgrade, iPhone client, direct P2P media lane.

### If the goal is two runtime families under one broader system

Then yes, two repos are justified, but only with a hard boundary:

- `agent` owns first-party runtime/platform concerns.
- `codexw` owns Codex-specific runtime/deployment concerns.

In that model, stop treating them as two competing full-stack products.

## Recommended boundary

### `agent` should own

- `agent_core`, `agentd`, and any custom multi-provider runtime semantics
- the general-purpose WebUI and operator workflows
- features that are runtime-agnostic across backends
- embedded/portable runtime work

### `codexw` should own

- integration with upstream `codex` app-server
- the wrapper-local API contract
- resume/handoff/upgrade logic for Codex deployments
- iOS app and direct peer media path
- Codex-specific supervision/status semantics

### What should not be owned twice

- generic broker identity model
- session lease vocabulary
- event envelope semantics
- artifact-contract definitions
- deployment inventory / rollout concepts
- remote shell/service inspection semantics

Those need one system of record, or you will keep paying translation cost forever.

## Practical recommendation

My recommendation is:

1. Keep both repos for now.
2. Stop expanding both as independent control planes.
3. Pick one repo as the broker/control-plane authority within the next decision cycle.
4. Treat the other repo as a runtime/backend that exposes a stable adapter.

Based on the current docs, the least-disruptive near-term interpretation is:

- `codexw` remains the authoritative runtime for upstream Codex deployments
- `agent` remains the broader custom-agent platform and consumer-facing WebUI/operator surface

That matches the existing sibling-handoff documents better than trying to merge them immediately.

## Bottom line

Today: yes, both repos can be justified.

Long term: no, not as two independent end-to-end platforms. You need one authoritative control plane and one or more runtime backends.
