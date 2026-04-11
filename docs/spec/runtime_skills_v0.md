# Runtime Skills System (v0)

Date: 2026-04-10
Status: draft (catalog tooling, agentd APIs, WebUI workflow integration, and broker team-run integration implemented)

## Summary

This spec defines a **runtime skills** layer for this repo.

A runtime skill is a **declarative reuse bundle** that composes existing
runtime primitives:

- tools
- tool plugins / tool servers
- workflow templates
- team templates
- policy presets
- instruction fragments

Runtime skills are **not** executable plugins and **not** repo-transform
packages. They sit above both and give operators a reusable, auditable way to
apply common agent/team/workflow patterns without inventing a second plugin ABI.

## Motivation

This repo already has two extensibility mechanisms:

1. **Tool plugins / tool servers** for executable runtime capabilities
   (`docs/TOOLS.md`)
2. **Repo skills** for auditable codebase transforms
   (`docs/spec/skills_system_v0.md`, `tools/skills/README.md`)

What is still missing is a first-class reuse layer for runtime behavior:

- "researcher-writer-reviewer"
- "broker fan-out with quorum approval"
- "coding task with test gate"
- "moderated handoff workflow"

Those should not require:

- a new plugin binary
- a custom fork of the workflow JSON
- copy/pasted prompts and policy blobs

## Goals

- Provide a **declarative runtime reuse layer**.
- Reuse existing tools/plugins/workflow/team infrastructure rather than
  replacing it.
- Keep skills **auditable** and **portable** across agentd/broker/WebUI.
- Make common operator patterns easy to load from CLI and WebUI.
- Preserve policy enforcement and approval semantics.

## Non-goals

- Replacing tool plugins or tool servers.
- Replacing repo-transform skills under `tools/skills/`.
- Making MCP mandatory.
- Building a full marketplace or dependency solver in v0.
- Allowing arbitrary code execution from runtime skill manifests.

## Terminology

- **Tool plugin / tool server**: executable capability extension that registers
  tools into the runtime.
- **Repo skill**: auditable package that patches or scripts this repo itself.
- **Runtime skill**: declarative bundle that references existing runtime
  capabilities and templates.
- **Skill application**: resolving a runtime skill into a concrete run/team/
  workflow submission.

## Layered extensibility model

This repo should use three layers:

1. **Plugin / tool server layer**
   - owns executable capabilities
   - owns runtime integration boundaries
   - owns security / sandbox / dependency isolation

2. **Runtime skill layer**
   - owns reusable behavior bundles
   - references tools/plugins/workflows/policies
   - does not execute arbitrary code directly

3. **Repo skill layer**
   - owns auditable repo transforms and guided upgrades
   - remains under `tools/skills/`

This prevents the two common failure modes:

- stuffing prompt/workflow reuse into plugins
- overloading "skills" to mean both runtime behavior and repo patching

## Core design rule

Runtime skills must be **pure data + references**.

They may reference:

- built-in tools
- plugin-provided tools
- tool servers
- workflow specs/templates
- team/orchestrator templates
- policy profiles
- instruction fragments

They may **not** include:

- shell hooks
- arbitrary scripts
- native library loading
- hidden execution side effects

If code needs to run, that belongs in a plugin or tool server.

## Runtime skill manifest

Suggested JSON shape:

```json
{
  "skill_id": "researcher-writer-reviewer",
  "version": "0.1.0",
  "description": "Three-role review pipeline with approval gate",
  "kind": "team_bundle",
  "requires": {
    "tools": ["fs_read", "text_search", "ui_action"],
    "features": ["team_runs", "approval_queue"],
    "plugins": []
  },
  "inputs_schema": {
    "type": "object",
    "properties": {
      "goal": { "type": "string" }
    },
    "required": ["goal"]
  },
  "instruction_fragments": {
    "shared": ["Be concise.", "Cite evidence."],
    "roles": {
      "researcher": ["Search and collect facts."],
      "writer": ["Draft output from evidence."],
      "reviewer": ["Reject unsupported claims."]
    }
  },
  "policy_preset": {
    "approval_mode": "tool_gate",
    "max_steps": 40
  },
  "team_template": {
    "roles": ["researcher", "writer", "reviewer"],
    "quorum": {
      "mode": "strict",
      "rule": "reviewer_must_approve"
    }
  },
  "ui": {
    "label": "Researcher / Writer / Reviewer",
    "category": "team-patterns"
  }
}
```

## Required fields

- `skill_id`: stable identifier
- `version`: semver-like version
- `description`: human-readable summary
- `kind`: coarse classification such as:
  - `instruction_pack`
  - `workflow_bundle`
  - `team_bundle`
  - `policy_bundle`

## Optional fields

- `requires.tools`: named tools that must exist
- `requires.plugins`: named plugin/tool-server packages expected to be present
- `requires.features`: runtime feature flags or server capabilities
- `inputs_schema`: JSON-schema input form for WebUI/CLI rendering
- `instruction_fragments`: shared or role-scoped instruction snippets
- `workflow_template`: embedded or referenced workflow template
- `team_template`: embedded or referenced team/orchestrator template
- `policy_preset`: defaults for approvals, budgets, automation mode, etc.
- `ui`: display metadata, categories, icons, or small-form hints

## Resolution model

Applying a runtime skill means:

1. load manifest
2. validate `requires.*` against the current runtime/broker/WebUI capability set
3. collect user/operator inputs against `inputs_schema`
4. materialize the concrete run/team/workflow request
5. persist the resolved skill metadata into run history for replay/audit

For `workflow_bundle` skills, the v0 materializer currently:

- deep-copies `workflow_template`
- merges user inputs into top-level `inputs`
- copies `policy_preset.max_steps` into `defaults.max_steps` when absent
- attaches a `runtime_skill` audit block with `skill_id`, `skill_version`,
  `manifest_sha256`, and resolved inputs

For `team_bundle` skills, the v0 materializer currently:

- accepts either a compact `team_template` object or a wrapped
  `team_template.{run,team}` payload
- synthesizes `run.prompt` from `inputs.goal` plus optional `deliverable` when
  the manifest does not supply a prompt directly
- derives `team.role_instructions` from `instruction_fragments.shared` plus
  `instruction_fragments.roles` when the manifest omits them
- seeds `team.roles` from the derived role-instruction keys when absent
- seeds `team.goal_contract` from `goal` and `deliverable` when absent
- copies `policy_preset.max_steps` into `run.max_steps` when absent
- enables `team.quorum_policy.mode=auto` when the manifest implies approval or
  quorum gating but leaves the execution policy unset
- attaches `team.runtime_skill` audit metadata with `skill_id`,
  `skill_version`, `manifest_sha256`, and resolved inputs

Resolution must fail early if:

- a required tool is missing
- a referenced plugin/tool server is not present
- a required runtime feature is unavailable
- inputs fail schema validation

## Persistence and audit

When a runtime skill is used, the system should persist:

- `skill_id`
- `skill_version`
- the fully resolved manifest snapshot or normalized hash
- the input values used to materialize the request

This should be attached to:

- workflow metadata
- team-run metadata
- broker relay audit / event records where appropriate

That preserves replayability and makes skill-based runs understandable after the
fact.

## Security model

Runtime skills do not weaken policy enforcement.

The runtime still enforces:

- tool allowlists
- approval requirements
- sandbox policies
- budget limits
- broker authorization and memberships

A skill may request capabilities, but it cannot bypass the existing control
plane.

## Loading model

Reasonable v0 search order:

1. repo-local runtime skill catalog
   - `tools/runtime_skills/`
2. operator state dir
   - `state/runtime_skills/`
3. optional broker-managed registry (future)

The v0 implementation does not need a network package manager.

Current v0 implementation status:

- local CLI tooling exists under `tools/runtime_skills/`
- agentd exposes:
  - `GET /api/v1/runtime_skills`
  - `POST /api/v1/runtime_skills/resolve`
- the WebUI workflow composer can now start from `workflow_bundle` skills
- the broker team-run UI can now start from `team_bundle` skills

## WebUI / CLI UX

Expected UX:

- WebUI can list skills by category and render input forms from
  `inputs_schema`
- CLI can materialize a skill by `skill_id` plus JSON/YAML inputs
- Team/workflow panels can offer "start from skill" alongside raw JSON mode

Implemented now:

- the workflow composer consumes the local daemon catalog for `workflow_bundle`
  entries and materializes them directly into composer JSON
- the broker team-run panel consumes the same daemon catalog for `team_bundle`
  entries and materializes them into the broker team-run form while preserving
  `runtime_skill`, `goal_contract`, and other hidden team/run payload fields

This gives a shorter path for common cases without taking away the low-level
power-user surface.

## Current v0 tooling

The repo now ships a local data-only catalog + CLI under `tools/runtime_skills/`:

- `validate_manifest.py`
  - validates runtime skill manifests without executing code
- `list_skills.py`
  - lists the discovered catalog entries in the v0 search order
- `resolve_skill.py`
  - materializes a runtime skill by `skill_id` plus JSON inputs
  - YAML input files are also supported when `PyYAML` is installed
- `create_skill.py`
  - scaffolds a local runtime skill under `tools/runtime_skills/local/`
- `catalog/`
  - example shipped runtime skills

This is intentionally a lightweight operator surface. It does not yet imply a
broker-native registry, remote package distribution, or server-side form
rendering beyond the current daemon-backed resolve flow.

## Interaction with MCP

MCP is not a product extension target for this repo.

The supported runtime extension path remains:

- built-in tools
- local plugins
- strict JSON-lines tool servers

If the repo later needs MCP interoperability for a concrete integration, treat it
as an **adapter layer** around tools/resources, not as the definition of the
runtime skill model and not as a second plugin system.

Runtime skills should remain valid whether the underlying tool came from:

- a built-in tool
- a local plugin
- a tool server
- an MCP bridge

## Example uses

- Research / draft / review team with strict approval
- Broker runtime-member allocation template
- Coding workflow with test gate + artifact diff + approval queue
- Incident triage workflow with memory search + trace correlation
- Connector onboarding skill that depends on a specific plugin/tool server

## Relationship to existing specs

- `docs/spec/skills_system_v0.md`
  - remains the spec for repo-transform skills
- `docs/TOOLS.md`
  - remains the executable capability extension surface
- `docs/spec/team_orchestration_v0.md`
  - runtime skills may resolve into team templates defined by this orchestration model
- `docs/spec/webui_workflow_graph_editor_v1.md`
  - WebUI can load runtime-skill materializations into the existing editor

## Acceptance criteria

- Spec is linked from `docs/spec/README.md`.
- The meaning of "runtime skill" is explicitly distinct from "repo skill".
- No new plugin ABI is introduced for runtime skill packs.
- Skills can be validated without executing arbitrary code.
