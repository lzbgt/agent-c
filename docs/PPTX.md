# PPTX Artifacts (Generate + Serve + Preview) — Draft

Date: 2026-01-30

This document defines an end-to-end workflow where an agent:

1) generates a real `.pptx` on the host (agentd tools root)
2) registers it as an artifact so the UI can download it (`/api/v1/file`)
3) (optionally) presents richer UI via client collaboration RPC (`dom_apply` / entities)
4) stops deterministically using the DoD handshake (`artifact_rendered` / `client_rpc_*`)

This is intentionally **power-oriented**: the goal is not a “toy demo”, but a repeatable pattern for agents producing real
documents and presenting them through the collaboration surface.

## Goals

- **Real file artifact**: a standards-compliant `.pptx` that opens in PowerPoint/Keynote/LibreOffice (best-effort).
- **Downloadable** in the Web UI via the existing daemon file server (`GET /api/v1/file?path=...`).
- **Previewable** in the Web UI without requiring external CDNs:
  - for v1: extract slide text and render a lightweight preview.
- Deterministic DoD so the agent stops repeating:
  - use `artifact_register` + wait for `artifact_rendered`
  - or use `client_rpc` with correlated progress/result events

## Non-goals (v1)

- Pixel-perfect slide rendering in browser.
- Full fidelity shapes/images/animations.
- “Bypass” autoplay/permission policies (not relevant for PPTX).

## Host-side generation (framework-first)

This repo intentionally does **not** hardcode a generator for every file type.
Instead, the agent should treat “generate a PPTX” as a normal tool-using task:

1) pick an appropriate generator toolchain (Python library, Node library, external binary, etc.)
2) if missing, install it autonomously into a local, gitignored deps folder (see default system prompt guidance)
3) generate the file, then register it as an artifact so the UI can render/download it

Example approaches an agent can choose (non-exhaustive):
- install a Python PPTX library into `./.agent_deps/py` and use it to generate a `.pptx`
- install a Node PPTX library into a local `.agent_deps/node` workspace and run it
- use a native binary generator if available on the host

## Serving the file to the UI

The daemon already serves files via:

- `GET /api/v1/file?path=<relative-or-absolute>&yolo=0|1`

The UI already renders unknown artifacts as a download link, so `.pptx` is usable even without any special UI code.

Note: returning the correct MIME type is helpful for browsers and preview libraries. This repo includes a minimal MIME map
for common artifact types (including `.pptx`), and agents may also override MIME explicitly via `artifact_register(mime=...)`.

## Agent responsibility vs framework responsibility

Framework responsibilities (agentd + client protocol):
- serve the bytes (`/api/v1/file`)
- allow the agent to register artifacts (`artifact_register`)
- allow the agent to create/modify client surfaces via RPC (`client_rpc`, `dom_apply`, `entity_apply`, `page_eval`)

Agent responsibilities (LLM backend behavior):
- decide *how* to generate the PPTX (choose/install a toolchain)
- decide *how* to present/preview it (simple download link, or client-side preview library)
- decide a deterministic DoD and stop (wait for `artifact_rendered` / `client_rpc_result` / `client_rpc_progress`)

## Optional: richer presentation using client RPC

If the agent wants a more explicit UI surface than the default artifact card, it can:

- create an entity (client-agnostic) via `rpc.kind="entity_apply"`
- or patch DOM via `rpc.kind="dom_apply"` (Web UI-specific)

For example, a `dom_apply` can append a link to the PPTX file endpoint using the known artifact path.

## UI preview strategy (v1)

To avoid hardcoding “pptx played” or relying on CDNs, the UI can:

1) fetch the `.pptx` bytes from `/api/v1/file`
2) unzip client-side
3) extract `ppt/slides/slide*.xml`
4) extract text runs (`<a:t>…</a:t>`) and render a lightweight preview

This is a client implementation detail; other clients (Slack/mobile) can map PPTX entities to their own surfaces.

## Relationship to client RPC and entities

A PPTX is just one example of a “host-produced artifact”. The general pattern remains:

- Generate the artifact with any suitable toolchain (possibly after installing dependencies).
- Use `artifact_register` so the UI can render it explicitly and provide a download link.
- Optionally use client RPC (`dom_apply`) or entities (`entity_apply`) to build richer UI around it.
- Use client acknowledgements (`artifact_rendered`, `client_rpc_result`, `client_rpc_progress`) as the DoD stop condition.

## Power note: client-loaded preview libraries

For richer previews (true slide rendering), the **agent** can use the collaboration RPC to create a preview surface in the client:

- Use `rpc.kind="dom_apply"` or `rpc.kind="page_eval"` to:
  - create a container element
  - dynamically load a JS preview library (CDN or bundled)
  - render the PPTX into the container

This keeps the framework generic: the daemon serves bytes; the agent composes client-side capabilities to achieve the desired UX.

## Deterministic DoD (stop conditions)

Recommended patterns:

1) Agent creates the PPTX.
2) Agent calls `artifact_register(path="out/hello_world.pptx", kind="file")`.
3) Agent waits once:
   - `client_wait_event(type="artifact_rendered", data_match={tool_call_id:"<artifact_register tool_call_id>"})`
4) Agent stops (or continues to the next task).

See `docs/DOD_ACK.md` for the general handshake rules.
