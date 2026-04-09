# agent vs popular GitHub agentic frameworks

Date: 2026-04-10  
Author: Codex  
Scope: compare this repo against active, popular GitHub agentic frameworks/products, then identify ideas worth borrowing.

## Executive answer

This repo is **not** in the same category as most popular agentic frameworks.

Most popular projects are primarily:

- an **SDK/framework** you embed into an app (`LangGraph`, `PydanticAI`, `CrewAI`, `Mastra`, `Microsoft Agent Framework`)
- or a **specialized coding-agent product/runtime** (`OpenHands`)

This repo is closer to a **self-hostable agent operations platform**:

- portable core + daemon + broker + CLI + WebUI
- durable workflows and team orchestration
- auth, routing, audit/event replay, approvals, and OTA
- operator-facing panels for memory, traces, workflows, broker teams, and approvals

That is a stronger operational story than most SDK-first frameworks, but it also means this repo currently gives up some things those frameworks do better:

- tighter typed agent/workflow authoring contracts
- more polished developer studios / visual debugging
- stronger benchmark/eval productization
- cleaner protocol-level interoperability for agent-to-agent and tool ecosystems

## Method

Selection criteria:

- active or still widely influential public GitHub projects
- clearly positioned as agentic frameworks/products
- enough adoption to matter in practical design choices

Current-source snapshot:

- repo facts were pulled on 2026-04-10 from GitHub API and official READMEs
- full upstream source snapshots were saved under [`docs/research/sources/agent-frameworks-20260410`](./sources/agent-frameworks-20260410)

Important caveat:

- `AutoGen` is included as a historical reference because it remains influential, but its own README says it is in maintenance mode and recommends `Microsoft Agent Framework` for new projects.

## What this repo is, factually

From repo docs, this project explicitly positions itself as:

- a **production-oriented agentic platform** with portable core, daemon, broker, CLI, and WebUI
- durable workflows with **DAGs + idempotency**
- team orchestration with role plans, approvals, runtime members, and replayable events
- policy/limits hooks, memory tooling, operator panels, and OTA

Evidence:

- [`README.md`](../../README.md)
- [`docs/BROKER.md`](../../docs/BROKER.md)
- [`docs/WEBUI.md`](../../docs/WEBUI.md)
- [`project.md`](../../project.md)

The architectural center of gravity is operational control:

- `agentd` owns HTTP/SSE APIs, persistence, workflows, approvals, diagnostics
- `broker` owns auth, multi-deployment routing, audit/event replay, team runs, quorum flows
- `WebUI` already exposes memory, traces, workflows, approval queues, broker/team/orchestrator panels

This matters because it means the right comparison is not "which prompt DSL looks nicest?" but:

- how much of the agent lifecycle is built into the product
- how much of security, replay, persistence, and operations is first-class
- where better developer ergonomics from popular frameworks should be imported

## Comparison snapshot

Star counts below are from GitHub API snapshots saved on 2026-04-10.

| Project | Stars | Main language | What it primarily is | Strongest angle |
| --- | ---: | --- | --- | --- |
| This repo | n/a | C + C++ + Go + TS | self-hostable platform | ops, relay, workflows, teams, WebUI |
| [LangGraph](https://github.com/langchain-ai/langgraph) | 28,803 | Python | low-level orchestration framework | durable stateful graph runtime |
| [CrewAI](https://github.com/crewAIInc/crewAI) | 48,458 | Python | multi-agent framework + commercial control plane | simple crews/flows mental model |
| [PydanticAI](https://github.com/pydantic/pydantic-ai) | 16,197 | Python | typed production agent framework | type safety + evals + interop |
| [Mastra](https://github.com/mastra-ai/mastra) | 22,836 | TypeScript | TS framework for agents/apps | TypeScript DX + built-in eval/obs |
| [Microsoft Agent Framework](https://github.com/microsoft/agent-framework) | 9,219 | Python | multi-language agent/workflow framework | graph workflows + DevUI + checkpoints |
| [OpenHands](https://github.com/OpenHands/OpenHands) | 70,908 | Python | coding-agent product/runtime | coding-agent UX + benchmark discipline |
| [AutoGen](https://github.com/microsoft/autogen) | 56,876 | Python | historic multi-agent framework | influential patterns, Bench, Studio |

## Where this repo differs

### 1. This repo is platform-first; most of the field is framework-first

`LangGraph`, `PydanticAI`, `CrewAI`, `Mastra`, and `Microsoft Agent Framework` are mainly things you **embed** into an app or service. This repo already ships:

- a daemon
- a relay/broker
- a browser UI
- deployment/verification workflows

That makes this repo more opinionated and more operationally complete than most of the comparison set.

Implication:

- If a user wants to build one agent app inside an existing web service, these frameworks are usually lighter.
- If a user wants a durable, auditable, self-hosted operator surface with multi-deployment routing and team orchestration, this repo is structurally ahead.

### 2. This repo has a stronger multi-tenant relay/control-plane story than the SDKs

`docs/BROKER.md` already documents:

- OIDC/JWT user auth
- DB-backed agent memberships
- secure relay to specific deployments
- audit/event replay
- persisted team runs, quorum approvals, runtime members
- broker-backed WebUI profiles and live SSE/event flows

That is not the default shape of `LangGraph`, `PydanticAI`, `Mastra`, or `CrewAI`. Those ecosystems usually assume you will build or buy your own control plane around the framework.

The closest comparison here is not LangGraph. It is the commercial sidecars around those frameworks:

- LangSmith deployment/studio
- CrewAI AMP / Control Plane
- OpenHands Cloud / Enterprise

This repo already owns more of that stack in-tree.

### 3. This repo is stronger on operator UX breadth than most frameworks

The WebUI already includes:

- memory tooling
- trace correlation
- workflow inspection and graph editing
- approval queues
- broker team/orchestrator controls
- live team-run status and broker event replay

Again, that is broader than a normal agent SDK. In practical terms, this repo behaves more like an **agent platform** than a library.

### 4. This repo is weaker on typed authoring ergonomics than the best newer frameworks

This is where `PydanticAI` and `Microsoft Agent Framework` are clearly ahead.

What they emphasize:

- typed graphs / typed outputs / typed capabilities
- clearer schema surfaces for agent definitions
- more guided developer tooling

This repo has OpenAPI surfaces and strong operational semantics, but the authoring experience is still more "powerful systems surface" than "tight typed application framework."

This is consistent with the recent UI typing cleanup work already underway in the repo.

### 5. This repo has durable execution, but not yet the same developer-debugger framing as LangGraph/MAF

This repo already has durable workflows, idempotency, replay, approval flows, and structured memory checkpoints.

But `LangGraph` and `Microsoft Agent Framework` present durability as a **developer mental model**:

- graph node/state model
- checkpointing as a first-class authoring primitive
- human interrupt / resume / time-travel as explicit workflow concepts

This repo has parts of the operational substrate already, but the developer-facing abstraction and debugger story is less consolidated.

### 6. OpenHands is a different animal

`OpenHands` is not the best comparison for general orchestration, but it is a very relevant comparison if this repo wants to win coding-agent workloads.

OpenHands is strong at:

- coding-agent ergonomics
- benchmark culture
- cloud/local/enterprise packaging
- task-oriented agent UX for software engineering

This repo is broader than OpenHands, but if the product goal includes "best coding agent platform," OpenHands has stronger specialization today.

## Framework-by-framework verdict

### LangGraph

What they do better:

- clearer state graph model
- stronger developer framing for checkpoints, interrupts, memory, and long-running agents
- better ecosystem story around evals/observability/deployment through LangSmith

Where this repo is stronger:

- built-in daemon + broker + UI
- auth, memberships, audit trails, event replay
- team/quorum/runtime-member operations
- OTA and deployment continuity checks

Takeaway:

- borrow the **developer model**, not the platform shape

### CrewAI

What they do better:

- easier onboarding and simpler mental model: `Crews` and `Flows`
- faster path from zero to visible multi-agent behavior
- clearer commercialization of control-plane features

Where this repo is stronger:

- more real relay/auth/audit infrastructure in-tree
- better secure multi-deployment story
- broader operator-facing product surface

Takeaway:

- borrow the **authoring ergonomics** and template simplicity

### PydanticAI

What they do better:

- type-safe agent authoring
- stronger contract discipline for outputs, tools, and capabilities
- explicit evals, observability, MCP, A2A, UI event streams, and approval hooks
- YAML/JSON agent spec support

Where this repo is stronger:

- fuller operational platform
- richer self-hosted control plane
- stronger deployment topology story

Takeaway:

- borrow the **typed contract and protocol interop discipline**

### Mastra

What they do better:

- modern TypeScript developer experience
- nice graph/workflow syntax
- built-in evals and observability positioning
- better fit for teams building TS-native agent apps

Where this repo is stronger:

- brokered operations, teams, audit/event replay, and approval flows
- stronger out-of-the-box operator UI breadth

Takeaway:

- borrow the **TypeScript DX patterns**, not the hosted/product packaging

### Microsoft Agent Framework

What they do better:

- multi-language framing
- graph workflows with streaming, checkpointing, HITL, and time-travel
- explicit `DevUI`
- benchmark/research lane via AF Labs

Where this repo is stronger:

- tighter integrated broker/control-plane model
- more complete web operator workflows for this exact stack

Takeaway:

- borrow the **checkpoint/time-travel debugger + DevUI discipline**

### OpenHands

What they do better:

- coding-agent user experience
- benchmark rigor
- product packaging for local/cloud/enterprise
- large-scale task execution framing

Where this repo is stronger:

- general agent platform surface beyond software-engineering tasks
- brokered multi-agent/team workflows and approval-routing model

Takeaway:

- borrow the **coding-agent benchmarking and sandbox specialization**

## Best ideas to borrow

These are the highest-leverage imports that fit this repo's current architecture instead of fighting it.

### Priority 0: make typed workflow/team authoring a first-class product surface

Inspiration:

- `PydanticAI` typed agents/capabilities/spec
- `Microsoft Agent Framework` graph workflows

Why it fits this repo:

- the repo already has OpenAPI surfaces
- the current UI typing cleanup is already moving toward schema-backed contracts
- workflows, team runs, approvals, and broker entities are strong candidates for versioned schema-first authoring

Concrete move:

- make workflow/team/team-run specs fully versioned JSON-schema/OpenAPI-first artifacts
- generate validators and client/server types from those schemas everywhere
- expose a "validated composer" mode in the WebUI instead of relying so heavily on raw JSON editing

### Priority 1: turn existing eval-pack infrastructure into a first-class operator feature

Inspiration:

- `OpenHands` benchmarks
- `AutoGen Bench`
- `Mastra` evals
- `PydanticAI` evals + Logfire
- `LangGraph` ecosystem via LangSmith

Why it fits this repo:

- this repo already has eval packs and verify flows (`docs/OPERATIONS.md`, `tools/eval_pack.py`, `tools/run_eval_pack_set.sh`)
- the missing piece is productization, not foundation

Concrete move:

- add a WebUI/CLI/operator view for eval packs, baselines, deltas, and regressions
- make trace, workflow, and team-run outcomes link directly into eval results
- support scheduled benchmark/soak runs through broker/agentd instead of keeping evals mostly as tooling

### Priority 2: add a checkpoint/time-travel workflow debugger

Inspiration:

- `LangGraph` durable execution + interrupts
- `Microsoft Agent Framework` checkpointing + time-travel + DevUI

Why it fits this repo:

- this repo already has durable workflows, event replay, and structured memory checkpoints
- the missing piece is a unified developer/operator debugger

Concrete move:

- add a WebUI "run debugger" view that can inspect workflow step state over time
- allow pause/resume/replay from checkpoints where the underlying workflow model permits it
- unify trace, run diff, approvals, and memory checkpoints into one debugging workflow

### Priority 3: standardize interop boundaries around MCP/A2A-style contracts

Inspiration:

- `PydanticAI` MCP + A2A
- `Microsoft Agent Framework` cross-runtime interoperability
- `Mastra` MCP server authoring

Why it fits this repo:

- the repo currently has strong internal control-plane semantics
- it does **not** appear to present MCP/A2A as a first-class external integration boundary in the main docs/code paths

Concrete move:

- add broker/agentd adapters for MCP-style tool surfaces and agent-to-agent RPC contracts
- use them first around team/orchestrator boundaries and external tool server registration

### Priority 4: ship clearer "fast path" templates for common multi-agent patterns

Inspiration:

- `CrewAI` crews/flows
- `Mastra` templates

Why it fits this repo:

- this repo is powerful but heavy
- new users face a larger conceptual surface area than they do in CrewAI or Mastra

Concrete move:

- package 5 to 10 canonical templates: researcher-writer-reviewer, quorum approval pipeline, broker fan-out, coding task delegation, moderation workflow, trace triage workflow
- make them one-click in the WebUI and one-command in CLI

### Priority 5: strengthen the coding-agent specialization lane without collapsing the platform into it

Inspiration:

- `OpenHands`

Why it fits this repo:

- this repo already has browser/client RPCs, tool runtime, workflows, and daemon control
- it can support coding agents without becoming coding-agent-only

Concrete move:

- define sandbox presets for code execution, repo mutation, test-running, and browser automation
- add benchmark tasks for SWE-style flows
- make artifact diffs, test evidence, and approval gates the default coding workflow surface

## Extensibility choice: plugins vs skills

This is the more important design choice than MCP support.

### Recommendation

- keep **plugins/tool servers** as the runtime execution extension mechanism
- keep MCP/A2A as **optional adapters**, not a core architectural dependency
- add **runtime skills** as a higher-level composition/reuse layer on top of existing tools/plugins
- keep the current `tools/skills/` system for what it already is: **repo transform packages**

### Why this is the right split for this repo

The repo already has a strong runtime extension model:

- `docs/TOOLS.md` defines **tool plugins** (in-process ABI)
- `docs/TOOLS.md` also defines **tool servers** (out-of-process JSON-lines)
- the broker already has a connector/plugin registry direction

The repo also already has a **skills** concept, but it is not a runtime agent-skill system today:

- `tools/skills/README.md` defines skills as auditable repo change packages with manifests, patches, preview, backups, and audit records

So the clean conclusion is:

- **plugins** are for executable capability extension
- **repo skills** are for codebase transformation and guided upgrades
- if we want OpenClaw-style skill reuse, we should add a **new runtime-skill layer**, not overload either existing concept

### What a runtime skill should be

A runtime skill should not be another binary/plugin ABI. It should be a reusable declarative bundle that composes existing system pieces:

- instructions / prompt fragments
- workflow templates
- team templates / role layouts
- policy presets
- allowed tool sets
- optional UI hints or operator forms

That gives the reuse value of a skill system without duplicating the plugin layer.

### What a plugin should stay responsible for

Plugins/tool servers should stay focused on things that need code and execution:

- new tools
- new device bridges
- new channel/connectors
- new sandbox adapters
- new provider/runtime integrations

If prompt logic, workflow composition, and operator policy start living inside plugins, extensibility gets harder, not easier.

### Minimal layered model

The clean model for this repo is:

1. **Plugin / tool server layer**
   - executable capabilities
   - stable ABI / protocol
   - security and sandbox boundaries

2. **Runtime skill layer**
   - reusable behavior packages
   - references tools/plugins but does not replace them
   - importable/exportable templates for common agent/team patterns

3. **Repo skill layer** (already exists)
   - auditable repo transforms under `tools/skills/`
   - upgrade/install/customization workflow for the codebase itself

### Why not make MCP mandatory

If MCP is not your preferred direction, that is a reasonable choice for this repo.

This project already has:

- its own daemon
- its own broker
- its own tool/plugin protocols
- a strong operator/control-plane identity

That means forced MCP adoption would mostly be an interoperability tax unless it unlocks concrete users or integrations. The correct posture is:

- support MCP only where it buys real leverage
- do not let it define the internal extensibility model

### Concrete next step if we choose skills

If you want to borrow one idea from OpenClaw/NanoClaw, it should be:

- add a **runtime skill manifest** and loader for reusable agent/team/workflow bundles

But do it as a thin declarative layer that references existing plugins, tools, and workflows.

Do **not** make it a second plugin system.

## What this repo should not copy

### Do not become SDK-only

The strongest differentiator here is the integrated daemon + broker + WebUI + operations posture. That is the moat.

### Do not over-couple to one framework ecosystem

`LangGraph`, `CrewAI`, and `Mastra` all have strong ideas, but this repo should keep its own runtime/control-plane identity instead of turning into a wrapper over someone else's orchestration model.

### Do not treat maintenance-mode projects as future architecture anchors

`AutoGen` still matters historically, but new design bets should anchor on active projects, especially `Microsoft Agent Framework`, not on maintenance-mode surfaces.

## Bottom line

This repo already wins on:

- operational completeness
- self-hosted control plane
- brokered multi-agent routing
- approvals, replay, and operator visibility

The best borrow opportunities are not "copy their agent loop."

They are:

1. stronger typed authoring contracts
2. first-class eval and benchmark productization
3. better debugger/studio UX for checkpointed workflows
4. cleaner interop protocols
5. simpler templates for common multi-agent patterns

If those are added, this repo would stop looking like "a powerful custom platform" and start looking like "a powerful custom platform with best-in-class developer ergonomics."

## Sources

Repo sources:

- [`README.md`](../../README.md)
- [`docs/BROKER.md`](../../docs/BROKER.md)
- [`docs/WEBUI.md`](../../docs/WEBUI.md)
- [`docs/OPERATIONS.md`](../../docs/OPERATIONS.md)
- [`docs/TOOLS.md`](../../docs/TOOLS.md)
- [`project.md`](../../project.md)
- [`tools/skills/README.md`](../../tools/skills/README.md)

Saved upstream snapshots:

- [`LangGraph README`](./sources/agent-frameworks-20260410/langgraph/README.md)
- [`CrewAI README`](./sources/agent-frameworks-20260410/crewai/README.md)
- [`PydanticAI README`](./sources/agent-frameworks-20260410/pydantic-ai/README.md)
- [`Mastra README`](./sources/agent-frameworks-20260410/mastra/README.md)
- [`Microsoft Agent Framework README`](./sources/agent-frameworks-20260410/microsoft-agent-framework/README.md)
- [`OpenHands README`](./sources/agent-frameworks-20260410/openhands/README.md)
- [`AutoGen README`](./sources/agent-frameworks-20260410/autogen/README.md)

Saved GitHub metadata snapshots:

- [`LangGraph repo.json`](./sources/agent-frameworks-20260410/langgraph/repo.json)
- [`CrewAI repo.json`](./sources/agent-frameworks-20260410/crewai/repo.json)
- [`PydanticAI repo.json`](./sources/agent-frameworks-20260410/pydantic-ai/repo.json)
- [`Mastra repo.json`](./sources/agent-frameworks-20260410/mastra/repo.json)
- [`Microsoft Agent Framework repo.json`](./sources/agent-frameworks-20260410/microsoft-agent-framework/repo.json)
- [`OpenHands repo.json`](./sources/agent-frameworks-20260410/openhands/repo.json)
- [`AutoGen repo.json`](./sources/agent-frameworks-20260410/autogen/repo.json)
