# Agent C documentation

Start here to choose a maintained guide. The [project README](../README.md)
introduces the product; this page owns documentation navigation and the
distinction between contracts, deployment instructions and recorded evidence.

## Start by task

| Task | Read first |
| --- | --- |
| Run the local 4B model on Arch | [AI Model deployment runbook](https://github.com/lzbgt/tiny-multimodal-edge-ai/blob/main/docs/local-4b-agent.md), then [local web integration](LOCAL_MODEL_WEB.md) for Agent C browser and adapter behavior |
| Build, verify or diagnose Agent C | [Operations](OPERATIONS.md), [diagnostics](DIAGNOSTICS.md), [limits](LIMITS.md) |
| Use the CLI or consume its events | [CLI](CLI.md), [CLI event protocol](CLI_EVENT_PROTOCOL.md) |
| Develop the browser workspace | [WebUI](WEBUI.md), [UI design](UI_DESIGN.md) |
| Configure production services | [Arch deployment and owner stop](ARCH_HOST_DEPLOYMENT.md), or [other deployment paths](DEPLOYMENT.md) |
| Understand architecture and implemented scope | [Current architecture](ARCHITECTURE_CURRENT.md), [source snapshot and qualification](CURRENT_SNAPSHOT.md) |
| Author durable processes | [SOP workflows](SOP_WORKFLOWS.md), [workflow engine](WORKFLOWS.md), [effect recovery](WORKFLOW_EFFECTS.md) |
| Extend host tools or understand retrieval evidence | [Tools](TOOLS.md) |
| Embed the portable core | [Embedded C API](EMBEDDED_C_API.md), [platform support](PLATFORM_SUPPORT.md) |
| Publish a change | [Publishing policy](PUBLISHING.md) |

The local model profile and the stopped production deployment are separate.
Source changes and documentation do not restart services or establish that an
installed binary is current. The [production hold](ARCH_HOST_DEPLOYMENT.md#owner-stop-2026-11-12)
remains effective.

## Which document owns a fact?

| Subject | Authority |
| --- | --- |
| Product direction and subsystem boundaries | [ARCHITECTURE_CURRENT.md](ARCHITECTURE_CURRENT.md); [DESIGN.md](../DESIGN.md) supplies the broader system map |
| Native tool schemas, cancellation, captured output, retrieval bounds, dates and citations | [TOOLS.md](TOOLS.md) |
| Local browser workflow, loopback adapter, authentication and forwarded API contract | [LOCAL_MODEL_WEB.md](LOCAL_MODEL_WEB.md) |
| Concrete Arch model/profile selection, launcher, ports, key locations and operation | [AI Model runbook](https://github.com/lzbgt/tiny-multimodal-edge-ai/blob/main/docs/local-4b-agent.md), maintained with its launcher and manifest |
| General host build, test and maintenance procedures | [OPERATIONS.md](OPERATIONS.md); deployment-specific steps belong in the appropriate runbook |
| Implemented scope, limitations and recorded qualification | [CURRENT_SNAPSHOT.md](CURRENT_SNAPSHOT.md), with explicit source/deployment distinctions |
| Compact orientation | [HANDBOOK.md](HANDBOOK.md), generated from [handbook/OVERVIEW.md](handbook/OVERVIEW.md); feature contracts take precedence |
| Dated research, decisions and historical outcomes | [Research](research/agent-systems-20261110.md), [archive](ARCHIVE.md), and linked reports; these do not redefine current defaults or live status |

When behavior changes, update its source and owning contract together. Link to
that contract from setup guides instead of copying its limits. Keep test
requirements separate from dated results, and preserve the revision, profile and
environment attached to evidence. Regenerate the handbook with
`python3 tools/build_handbook_bundle.py` after editing its overview.

## Runtime and tool references

- [Daemon](AGENTD.md): operations and runtime configuration.
- [Daemon library](AGENTD_LIB.md): embedding and tool-extension interfaces.
- [Protocol](PROTOCOL.md): run/artifact envelopes.
- [Tools](TOOLS.md): native tools, tool servers and plugin ABI.
- [Streaming](STREAMING.md): compatibility matrix and transport behavior.
- [Limits](LIMITS.md): host size/time caps and operator settings.
- [Autonomous work](AUTONOMOUS_WORK.md): requirements, dependencies and continuation.
- [Bound goals](GOAL_WORK_LEDGER.md): host authority and broker goal coordination.
- [Workflows](WORKFLOWS.md), [SOPs](SOP_WORKFLOWS.md), [decisions](WORKFLOW_DECISIONS.md),
  [effects](WORKFLOW_EFFECTS.md), [budgets](WORKFLOW_BUDGETS.md) and
  [device assignments](DEVICE_ASSIGNMENTS.md): their respective durable contracts.
- [Runtime skills](../tools/skills/README.md): skill manifests and auditable apply workflow.

## Storage, clients and operations

- [Database](DB.md), [diagnostics](DIAGNOSTICS.md) and [memory](MEMORY.md).
- [Client contract](CLIENT.md): events, UI actions, definition of done and RPCs.
- [Broker](BROKER.md) and [definition-of-done acknowledgements](DOD_ACK.md).
- [Local model web integration](LOCAL_MODEL_WEB.md), [WebUI](WEBUI.md) and [voice](VOICE.md).
- [Operations](OPERATIONS.md), [deployment](DEPLOYMENT.md),
  [Arch deployment](ARCH_HOST_DEPLOYMENT.md) and [devstack network/auth](DEVSTACK_NETWORK_AUTH.md).
- [Agentic vision](AGENTIC_VISION.md): expectations and acceptance principles.
- [Roadmap](../TODOS.md): remaining work.

## Platforms, specifications and references

- [Platform support](PLATFORM_SUPPORT.md), [macOS packaging](MACOS_PACKAGING.md),
  [embedded C API](EMBEDDED_C_API.md) and [ESP32-S3 maturity](ESP32S3_AGENT_CORE_MATURITY.md).
- [Edge interoperability](EDGE_INTEROP.md) and [AVM/OREN ecosystem](OREN_LANG_ECOSYSTEM.md).
- [Versioned specifications](spec/README.md), [OpenAPI index](openapi/README.md)
  and [vendored subtree policy](VENDORED.md).
- [Codexw broker/WebUI handoff specification](spec/codexw_broker_webui_handoff_v0.md):
  the versioned integration task list, distinct from current implementation evidence.
