# WebUI Workflow Graph Editor v1

Last updated: 2026-02-20

## Goals

- Provide a drag-and-drop workflow composer inside WebUI for fast DAG authoring.
- Support the two most common authoring cases:
  - LLM request tasks (`request.prompt` nodes).
  - Multi-agent collaboration via `kind:"agentd_parallel"`.
- Preserve JSON workflow submission as the source of truth (graph is a helper, not a new protocol).
- Keep the implementation dependency-light (no new graph library for v1).

## Non-goals (v1)

- Full fidelity editing of every workflow task kind.
- Automatic schema validation beyond basic structural checks.
- Visual debugging of runtime execution (handled elsewhere in WebUI).

## Constraints

- Must not break existing JSON composer flows.
- Must remain compatible with durable workflow submit schema.
- Avoid introducing heavy UI dependencies until the editor stabilizes.

## Data Model

Graph state (client-only):

- Nodes: `{ id, kind, x, y, prompt, targets[] }`
  - `kind` is `llm` or `agent_parallel`.
  - `prompt` stores the LLM prompt for local or remote nodes.
  - `targets[]` are base URLs for remote agentd targets (optional; falls back to runtime defaults).
- Edges: `{ from, to }` representing `depends_on` (from -> to).

## Serialization

Graph -> workflow JSON:

- For `llm` nodes:
  - `task_id = id`
  - `request.prompt = prompt`
  - `request.no_session = true`
- For `agent_parallel` nodes:
  - `kind = "agentd_parallel"`
  - `agentd_parallel.targets = targets` (or runtime defaults)
  - `agentd_parallel.agentd_call` uses `workflow_submit_and_wait` with a single-task workflow `RUN`.
  - `agentd_parallel.aggregate` defaults to `first_ok` with `value_pointer` pointing at `RUN` result.
- `depends_on` is derived from edges.
- Top-level `defaults` and `allow_inline_api_keys` follow the existing JSON composer behavior.

Workflow JSON -> graph:

- LLM tasks are detected via `request.prompt`.
- `agentd_parallel` tasks are imported when they match the editor's expected structure.
- Unsupported tasks are ignored with warnings.
- `broker_proxy` targets are normalized to a `base_url` when possible (warning emitted).

## UX

- Mode toggle: JSON or Graph.
- Graph mode includes:
  - Drag repositioning of nodes.
  - Click-to-connect dependencies (output handle -> input handle).
  - Node inspector for id/kind/prompt/targets.
  - Import from JSON and export to JSON (for advanced edits).
  - Auto-layout for quick organization.

## Security / Safety

- Graph editor never executes workflows on its own; submission still requires explicit user action.
- `agentd_parallel` remains gated by daemon flags (`--workflow-enable-http-tasks`).

## Future Enhancements

- Full task-kind palette (aggregate, memory, tools, deterministic tasks).
- Rich target editing (broker proxies, auth presets).
- Drag-to-connect from canvas to generate nodes.
- Per-node validation and inline error badges.
