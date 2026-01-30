# Client Profiles (System Prompt Extensions)

This project treats the **client** (Web UI, mobile app, Slack bot, etc.) as a **collaboration surface**: a stateful environment
where the agent and the human collaborate by creating/updating “entities” and/or patching UI elements.

To keep user prompts clean (no brittle “use tool X with schema Y” instructions), `agentd` can inject a **client profile**
as an additional system prompt snippet at the start of a session.

## What is a client profile?

A client profile is a short, client-kind-specific system instruction that answers:

- What presentation surface exists (e.g. Web UI has a “Scene” panel + history).
- Which interaction primitives exist (client RPC kinds supported).
- What “Definition of Done” (DoD) acknowledgements to wait for to avoid runaway loops.

Profiles are keyed by `client.kind` in the run request.

## Current behavior

- The Web UI includes a `client` object in run requests:
  - `client.kind = "webui"`
  - `client.id` is stable per browser profile
  - `client.instance_id` is stable per tab
- When starting a **new session** (empty transcript) and `tools="host"`:
  - `agentd` inserts `default_host_system_prompt()` (unless disabled)
  - `agentd` then appends `CLIENT_PROFILE=<kind>` when a profile exists (unless disabled)

Implementation: `daemon/src/client_profiles.cpp` and `daemon/src/run_endpoints.cpp`.

## DoD and acknowledgement events

Client profiles are responsible for steering the model away from “repeat the same UI action forever”.

For the Web UI profile, DoD is typically satisfied when the agent observes one of:

- `client_rpc_result` (correlated by `rpc_id`)
- `artifact_rendered`
- `ui_action_shown`

The agent can wait deterministically using `client_wait_event`, or join multiple waits using `client_wait_any/all`.

## Extending to new clients

Add a new profile in `daemon/src/client_profiles.cpp` keyed by the new `client.kind`, and ensure the client sends
the same `client` object with run requests (or equivalent metadata).

Note: The model can still discover runtime client capabilities at run-time via `client_peek` and/or a bounded
`client_rpc` (e.g. `state_snapshot`). Profiles should describe defaults and best practices, not hardcode every behavior.

## Web UI scene dimensions

For the Web UI, the recommended pattern is:
- the agent creates a `canvas2d` entity with explicit `props.width`/`props.height` (so dimensions are unambiguous)
- the agent provides drawing logic as `props.script` (JavaScript), which is executed with `(ctx, canvas, width, height, props, args)`

This keeps user prompts succinct (“draw a sine plot”), while the injected profile provides the operational details.
