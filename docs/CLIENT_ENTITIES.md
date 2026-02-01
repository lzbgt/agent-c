# Client Entities / Scene Model (Draft)

Date: 2026-01-30

`client_rpc` is the universal collaboration primitive, but *DOM mutation is not universal*:
DOM is Web-client-specific.

The universal abstraction is **entities**:

- The client maintains a set of **entities** (aka a “scene”).
- Entities are typed (`kind`) and have structured state (`props`).
- The agent can:
  - query entities
  - create entities
  - update entities
  - trigger actions on entities
  - remove entities / clear the scene

Different clients implement entities differently:
- Web UI: renders entities into React views (Canvas, cards, etc.)
- Slack: entities might map to message blocks and ephemeral attachments
- Mobile: entities map to native views

This is the real collaboration surface: it lets the agent create “meaningful things” (canvas plots, widgets) and clean them up deterministically.

## Durable Scene vs. client-local Scene

This repo has **two related “scene” concepts**:

1) **Client-local scene** (conceptual protocol)
- The client maintains entities and applies `entity_apply` ops to its own UI (web DOM, native views, Slack blocks, …).

2) **agentd durable Scene** (server-side persistence)
- The daemon can persist a per-session Scene snapshot in its DB and expose it via:
  - `GET /api/v1/session/scene`
  - `POST /api/v1/session/scene/apply`
- This is used to make Scene rendering refresh-proof in the WebUI (and can be reused by other clients).

Quick reference: `docs/CLIENT_AGENTD_SPEC.md`.

## RPC kinds

This repo uses client RPC kinds:

- `entity_query` (read-only)
- `entity_apply` (side-effecting; gated by client “side effects” setting)

Both are requested via `ui_action(type="client_rpc", rpc={...})` and acknowledged via `client_rpc_result`.

## `entity_query` (read-only)

Request:

```json
{
  "kind": "entity_query",
  "args": {
    "entity_kind": "canvas2d",
    "id_prefix": "plot-",
    "limit": 50
  }
}
```

Response (example):

```json
{
  "kind": "entity_query",
  "count": 1,
  "items": [
    {"id":"plot-1","kind":"canvas2d","title":"Sine plot","props":{...}}
  ]
}
```

## `entity_apply` (create/update/delete/action/clear)

Request:

```json
{
  "kind": "entity_apply",
  "side_effects": true,
  "args": {
    "ops": [
      {"op":"create","id":"plot-1","entity_kind":"canvas2d","title":"Sine plot","props":{"width":640,"height":240}},
      {"op":"action","id":"plot-1","action":"plot_sine","args":{"amplitude":1,"frequency":2,"phase":0,"samples":512}}
    ]
  }
}
```

Supported ops (v1):
- `create`: `{id?, entity_kind, title?, props?}`
- `update`: `{id, props}` (shallow merge)
- `delete`: `{id}`
- `clear`: `{entity_kind?}` (optional filter; **destructive**, see below)
- `action`: `{id, action, args?}` (client-defined)

Clients return an operation result list so the agent can debug and proceed deterministically.

## Example: draw a sine plot then clean up

1) Create canvas entity + render a sine plot:
- `entity_apply` with `create` + `action plot_sine`

2) Wait for the `client_rpc_result`:
- `client_wait_event(type="client_rpc_result", data_match={rpc_id:"..."})`

3) When done, clean the scene:
- Prefer: `entity_apply` with `{op:"delete", id:"plot-1"}`
- Optional: `{op:"clear", entity_kind:"canvas2d"}` but only if the client treats it as a **gated / debug** operation.
  - For durable server-owned Scenes, a “clear” wipe affects post-refresh state and should not be exposed as a one-click UI action.

## Relationship to scripts (`script_eval`)

`script_eval` can expose a scene API so scripts can:
- create/update/delete entities
- query and react to entity state

This keeps the “probe only what I care about” property while still grounding results in a deterministic, inspectable scene model.
