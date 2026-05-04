# codexw Broker/WebUI Handoff v0

Date: 2026-03-14
Status: draft handoff
Audience: the engineer/agent working in this `agent` repo on broker and WebUI

## Purpose

This document converts the sibling `codexw` handoff into concrete work for
this repo.

The goal is not to redesign `codexw`. The goal is to consume the `codexw`
broker-facing surface that already exists today and to avoid building the
WebUI against routes or semantics that `codexw` does not actually claim.

## Upstream source package

Use these sibling-repo docs as the upstream source of truth when implementing
the tasks below:

- `../codexw/docs/codexw-broker-integration-handoff.md`
- `../codexw/docs/codexw-broker-adapter-contract.md`
- `../codexw/docs/codexw-broker-adapter-status.md`
- `../codexw/docs/codexw-broker-proof-matrix.md`
- `../codexw/docs/codexw-broker-host-examination-matrix.md`
- `../codexw/docs/codexw-cross-deployment-collaboration.md`
- `../codexw/docs/codexw-broker-artifact-contract-sketch.md`
- `../codexw/docs/codexw-broker-artifact-implementation-plan.md`

Short version:

- broker-backed app/WebUI clients are first-class consumers of `codexw`
- remote host examination is currently shell-first
- cross-deployment `codexw` collaboration and explicit work handoff are now a
  separate native architecture track
- a stable artifact index/detail/content API does not exist yet
- the shared codexw broker now has neutral runtime-instance activity routes;
  agentd should feed those routes through its explicit connector adapter, not
  by pretending to be a codexw deployment

## What this repo should assume

This repo should treat the following `codexw` surfaces as real:

- session create / attach / list / inspect
- attachment renew / release
- transcript fetch
- SSE consumption with `Last-Event-ID` replay/resume
- orchestration status / workers / dependencies
- shell list / start / detail / poll / send / terminate
- service list / detail / attach / wait / run
- capability list / detail
- owner / observer / rival lease semantics
- v2 runtime-instance inventory, action metadata/audit, and read-only
  `/sessions` / `/events` activity surfaces

For `agentd` interoperability, this repo should expose the shared activity
contract through connector-owned routes:

- advertise `broker.runtime_capabilities.v1` with `surfaces.sessions`,
  `surfaces.session_create`, `surfaces.events`, `surfaces.files`,
  `surfaces.shell`, and `surfaces.proxy_http`
- advertise `surfaces.proxy_sse` only on the external local-API facade path,
  where codexw has a registered `deployment-local-api-base-url` and can stream
  a real `text/event-stream` response
- serve `GET /api/v1/runtime/sessions` from daemon sessions and durable
  workflows
- serve `GET /api/v1/runtime/events` from client events and workflow events
- serve codexw session-style file list/read routes from a bounded file root
- serve codexw session-style one-shot shell start/list/detail routes for iOS
  host inspection while rejecting interactive `send` until agentd owns a
  durable interactive terminal contract
- serve `GET /api/v1/runtime/status` as the read-only update/OTA readiness
  source for the broker's v2 runtime-instance status route. OTA-enabled status
  should include `update.candidate` data derived from the daemon's OTA target
  (`url`, `sha256`, `version`, `drain_timeout_ms`) so shared clients can show
  the exact update payload before the broker dispatches `runtime.update`.
- keep operator actions capability-gated. The default connector must not
  advertise `runtime.restart`, `runtime.update`, or `runtime.upgrade`.
  `runtime.update` may be advertised only when the native connector is launched
  with `AGENTD_CODEXW_RUNTIME_UPDATE_MODE=agentd_ota` and the local daemon OTA
  path proves the drain/update boundary through `GET /api/v1/ota/status`.
  `runtime.restart` may be advertised only when the native connector is launched
  with `AGENTD_CODEXW_RUNTIME_RESTART_MODE=agentd_ota` and
  `/api/v1/ota/status` reports `restart.enabled=true` with safe boundary
  `agentd_supervisor_restart_drain`; broker restart commands are then forwarded
  to daemon-owned `POST /api/v1/ota/restart`. `runtime.upgrade` remains
  unadvertised.
- keep `tools/verify_codexw_live_agentd_activity.sh` passing against the live
  codexw broker before claiming the shared iOS/WebUI activity path is
  production-ready

This repo should **not** assume:

- artifact index routes
- artifact detail routes
- artifact content/download routes
- generic remote filesystem browsing
- workspace dynamic tool resurrection

## Work package 1: Session lifecycle + lease UX

Implement the broker/WebUI path around the supported `codexw` session model.

Required work:

- add or tighten broker client adapters for session create / attach / inspect /
  renew / release
- make `session_id` the primary remote-control handle in UI state
- surface owner / observer / rival status in the WebUI instead of treating all
  attachments as equivalent
- handle `attachment_conflict` as a first-class UI state, not as a generic
  failure toast

Acceptance criteria:

- a user can create or attach to a `codexw` session through the broker-facing
  client flow
- the WebUI shows current lease mode and whether mutation is blocked
- renew and release are explicit user actions
- lease conflicts are distinguishable from transport failures

## Work package 2: SSE backbone + replay/resume

Use the existing `codexw` event stream as the live runtime backbone.

Required work:

- wire event subscriptions through a dedicated broker/WebUI session stream path
- persist and reuse `Last-Event-ID` across reconnects and page refreshes
- merge replayed history and live stream without duplicating visible events
- keep reconnect behavior explicit in UI state so gaps and resumed streams are
  diagnosable

Acceptance criteria:

- page refresh can recover the visible runtime stream without losing the active
  cursor
- reconnect after interruption uses `Last-Event-ID`
- replay + live append does not double-render already acknowledged events

## Work package 3: Shell-first host examination UX

Treat host examination as shell/service/transcript/event work, not as a file
browser.

Required work:

- expose shell list / start / detail / poll / send / terminate in broker mode
- make shell outputs inspectable in a dedicated UI flow rather than burying
  them only in generic transcript panes
- preserve the relationship between transcript turns and host shell actions
- keep language in the UI shell-first and avoid implying a stable artifact
  browser

Acceptance criteria:

- a user can start and inspect a remote shell session through the broker/WebUI
- shell output remains readable after refresh via transcript/event recovery
- no broker/WebUI page claims there is already a generic artifact browser

## Work package 4: Service/capability operator path

Implement the reusable service and capability surfaces as operator tools for
remote examination and control.

Required work:

- expose service list / detail / attach / wait / run in broker mode
- expose capability list / detail in a way that helps users understand what a
  remote `codexw` session can do
- connect service/capability detail screens to the session they belong to
- use these surfaces as the preferred explanation path before inventing new
  synthetic UI abstractions

Acceptance criteria:

- operators can inspect available services and capabilities from the WebUI
- attach/wait/run flows work without needing direct terminal access
- service/capability state is scoped to the relevant `codexw` session

## Work package 5: Artifact boundary handling

Keep artifact-heavy UX honest until `codexw` ships a real artifact API.

Required work:

- represent current results as transcript, shell, service, or event references
- do not invent client-side routes named like a stable artifact contract unless
  they are explicitly backed by server routes
- gather concrete missing artifact use-cases as requirement feedback instead of
  papering over them with ad hoc parsing

Acceptance criteria:

- the UI can still expose useful outputs without pretending a stable artifact
  API exists
- artifact-centric gaps are captured as explicit follow-up requirements
- no current broker adapter claim in this repo implies implemented artifact
  list/detail/content support

## Work package 6: Proof and regression coverage

Back the integration work with broker and WebUI proof, not only docs.

Required work:

- add broker/WebUI tests for session lifecycle against the `codexw`-style
  client surface
- add SSE replay/resume coverage including `Last-Event-ID`
- add UI or broker regression coverage for lease conflict handling
- add shell/service/capability coverage for broker-backed host examination

Acceptance criteria:

- broker/client tests prove the supported `codexw` workflow surface
- WebUI coverage proves reconnect, replay, and lease conflict behavior
- tests do not claim artifact route support that the upstream repo has not
  implemented

## Work package 7: Cross-deployment collaboration + work handoff

Treat codexw-to-codexw collaboration as the next explicit lane after the
single-deployment baseline, not as a manual operator convention.

Required work:

- add a broker/WebUI concept for cross-deployment work handoff between codexw
  deployments
- model handoff as its own replayable object rather than as plain transcript
  prose
- preserve source/target deployment identity and source/target session
  correlation in UI and broker state
- keep the first handoff design separate from the still-missing artifact API

Acceptance criteria:

- the broker/WebUI design can describe a source deployment, target deployment,
  handoff record, and acceptance/decline flow without inventing fake artifact
  routes
- deployment switching is not described only as a manual operator convention
- future UI work can hang on a real handoff lane rather than on ad hoc notes

## Suggested implementation order

1. Session lifecycle + lease UX
2. SSE replay/resume
3. Shell-first host examination
4. Service/capability operator path
5. Artifact boundary guardrails
6. Proof and regression coverage
7. Cross-deployment collaboration + work handoff

## Practical rule

If a requested broker/WebUI feature can be satisfied through session,
transcript, event, orchestration, shell, service, or capability surfaces, build
it now.

If it requires a stable artifact list/detail/content API, treat it as a
requirement back to `codexw`, not as something this repo should fake locally.

If it requires one codexw deployment to hand work to another deployment,
implement it as an explicit broker-mediated collaboration/handoff lane rather
than as an undocumented deployment switch or operator-only note.
