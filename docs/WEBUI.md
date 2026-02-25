# WebUI

The WebUI is a Vite-based static site that talks to `agentd` over HTTP. It can run in
direct mode (connects to agentd) or broker mode (OIDC + broker console).

## Dev (hot reload)

```bash
cd ui
npm install
npm run dev -- --port 5173
```

If you prefer port `8100`:

```bash
cd ui
npm run dev -- --port 8100
```

## Production build + serve

```bash
cd ui
npm ci
npm run build
npm run preview -- --host 127.0.0.1 --port 8100
```

For static hosting (nginx/Caddy/S3), serve `ui/dist/`.

## Runtime defaults (no rebuild)

Edit `ui/public/agentui-config.js` (copied to `ui/dist/agentui-config.js`) to prefill:
- `clientId` (client prefs id for server-side profile sync)
- `connectionMode` (`direct` or `broker`)
- `daemonBaseUrl`, `brokerBaseUrl`, `brokerAgentId`, `brokerDeploymentId`
- `daemonAuthToken`, `brokerAuthToken` (only if you accept putting tokens in a static file)
- `model`, `baseUrl`, `proxyUrl`, `timeoutMs`
- `tools`, `yolo`, `hostPolicy`, `verbose`
- `automationProfile` (full|guided|strict|custom; omit for daemon default)
- `allowClientRpcs`, `allowClientEffects`, `allowUnsafePageEval`
- `workflowAgentTargets` (array or CSV of agentd base URLs for collaboration templates)
- `workflowBearerEnv` (env var name used by `agentd_call` bearer_env in templates)
- `serverPrefsMode` (`off`, `auto`, `on`)

Build-time overrides (optional): `VITE_AGENTD_BASE_URL`, `VITE_BROKER_BASE_URL`, and `VITE_AGENTUI_*` (including `VITE_AGENTUI_SERVER_PREFS_MODE`).

## Broker mode and profiles

- The WebUI caches multiple **connection profiles** (direct or broker-backed) locally and can persist them server-side.
- Each profile can enable **profile-specific run settings** (model/provider, tool flags, run limits).
- Settings → Moderator lets operators publish moderator directives/tasks (nonblocking) stored as client events.
- Moderator publishing supports optional assignees/scope for targeted routing; empty assignees broadcast to all listeners.
- In broker mode, the Moderator panel can populate assignees from connected broker agents.
- Moderator tasks include a quick template for broker runtime member updates (tool server helper).
- Moderator events panel can load recent directives/tasks with optional auto-refresh.
- Assignee fields include quick role chips (planner/executor/critic) for fast targeting.
- Moderator event rows include a JSON expand toggle for full payload inspection.
- Moderator events panel includes a filter box to match by type/actor/content.
- Moderator event rows include copy buttons for summary/JSON.
- Moderator events can be pinned for side-by-side comparison.
- Pinned events persist per base URL + session in localStorage.
- Copy actions show a brief confirmation toast.
- Pinned events can be exported/imported as JSON.
- Pinned events include a compare view with side-by-side JSON.
- Compare view includes a diff-only toggle with combined JSON output.
- Diff-only mode lists key-level differences with A/B values.
- Key diff rows highlight changed values for quick scanning.
- Compare view includes a swap control for A/B selections.
- Keyboard shortcuts: Ctrl+Shift+S swaps A/B, Ctrl+Shift+D toggles diff-only (when Settings is focused).
- The broker console exposes agent membership management, audit events, team setup (members + quorum rules), team runs, and quorum approvals (Team console includes recent quorum requests and approvals).
- The Team console includes **orchestrator run** controls (create/list/update/heartbeat) and a **spawn request** panel (create/lookup/update/list).
- Team members editor supports broker agent/deployment pickers and a bulk add for connected agents.
- Team member rows support quick pause/resume (updates status via PATCH).
- Team members list supports bulk pause/resume controls.
- Team members list supports bulk remove of paused members (with confirmation).
- Team member rows support inline edit (role/status/weight/capabilities/backend overrides/meta plus agent/deployment reassignment).
- Team settings editor can update display name, tags, policy ref, shared memory scope, and meta JSON.
- Team settings editor includes a role overrides JSON field (stored in `meta.role_overrides`) used as team run defaults.
- Team settings editor includes a role plan editor (role graph + role instructions + prompt mode) stored in `meta.role_graph` / `meta.role_instructions`.
- Role instructions can include `{{goal}}` and are used to compose per-role prompts for team runs.
- Team runs support **inline approvals** (optional) to satisfy strict quorum rules at submit time; failed quorum responses surface rule evals.
- Inline approvals live in the Team console → Team run panel. Add `member_id` + decision (optional `rule_id`/reason) before Create run; approvals are sent under `team.approvals`, persisted, and the Run approvals panel auto-loads the run after submission.
- The Team run panel lists recent runs (status/mode/summary) and supports a live (SSE) toggle to refresh on broker events without polling.
- Team runs support **runtime members** for per-run team composition (ephemeral). Use runtime members to add/pause agents dynamically; "Save to team" persists them into the registry.
- Runtime member quick-add can allocate connected agents by the team’s role plan to cover missing roles.
- Team run panel can update runtime members for an existing run (replace/merge) after lookup; runtime member list includes quick pause/resume/remove actions.
- Team run panel supports async mode (nonblocking): broker dispatches member runs via `/api/v1/run_async`, persists job IDs, and status lookups show per-member job state.
- Team run panel can cancel async runs and surfaces the aggregated member job summary plus cancel results.
- Team run panel supports per-role run overrides JSON (applied before member overrides; allowlist enforced server-side).
- Team run panel can seed role overrides from team defaults (`meta.role_overrides`) when present.
- Team run status lookup surfaces applied overrides (`role_overrides_applied` / `member_overrides_applied`) with an expand toggle.
- Team run status lookup supports auto refresh on team run SSE events.
- Team run status lookup surfaces `member_sessions` for moderator broadcasts and includes a moderator panel to publish directives/tasks to selected roles or members.
- Team run moderator panel can load aggregated moderator events across member sessions (reload-safe).
- Server-side sync prefers the **daemon** (direct mode) or **broker** (broker mode) when supported.
  - Default: **auto** (syncs when the server advertises client prefs and auth allows; broker requires OIDC token).
  - The client prefs id defaults to `webui` and can be changed in Settings → Connection.
  - Toggle “Sync connection profiles to daemon/broker” in Settings → Connection to force on/off.
  - Only non-secret fields are stored (URLs/ids/profile names). Auth tokens remain local.
- Devstack OIDC helper: `tools/devstack_oidc_token.sh --state out/devstack_state.json` (prints a bearer token).

## Diagnostics

Settings → Diagnostics triggers:
- `/api/v1/diagnostics`
- `/api/v1/diagnostics/providers`
- `/api/v1/diagnostics/provider_test`

See `docs/DIAGNOSTICS.md` for provider_test usage.

## Approval queue

The WebUI includes an **Approval queue** panel for tool-level approvals:
- Lists approvals from `/api/v1/approvals` with basic filtering.
- Loads approval details + decisions from `/api/v1/approvals/<approval_id>`.
- Submits decisions via `/api/v1/approvals/<approval_id>/decisions`.

## Memory explorer

The **Memory explorer** panel (toggle from the main UI) provides operator tooling for durable memory:
- Structured memory query + trace correlation.
- Correlation index build (cross-run evidence linking).
- Memory index + salience snapshots.
- Recap generation + recap list filtering by `kind`.
- Broker console memory maintenance supports recap kind tagging + list filtering across deployments.
- Recap scheduling controls that update `memory.recap_*` config via `/api/v1/config/update`
  (scheduled recaps require `summary_model`).

## Run diff (replay bundles)

The WebUI includes a **Run diff** panel for replay bundle comparison:
- Load two `run_id` values and diff request/response/tool records from `/api/v1/run/replay`.
- Displays replay hash match status, usage summary, and a baseline shortcut.
- Baselines are stored in the browser (per base URL) for quick reuse.
- Optional evidence loader fetches DB-backed events/artifacts (`/api/v1/db/run`) plus attestation bundles
  (`/api/v1/run/attestation`) to diff event/artifact deltas and surface signed hashes.

## Workflows (read-only)

The WebUI includes a **Workflows** panel for durable workflow inspection:
- Lists recent workflows by status (`/api/v1/workflows`).
- Loads a workflow with tasks + DAG layout (`/api/v1/workflow`).
- Surfaces budgets/usage and optional spec/results when enabled.

This panel includes a **Workflow composer** with JSON and Graph modes:
- JSON templates for LLM DAG workflows (A→B/C) and `agentd_parallel` collaboration templates.
- JSON template includes an `agentd_parallel` demo preset with longer timeout and example goal input.
- JSON mode includes a “Demo → Graph” button to load the demo template directly into the graph editor.
- JSON mode includes a “Demo → Submit (wait)” shortcut to submit the demo and poll for completion.
- When waiting, the composer shows a lightweight status line with elapsed time.
- The status line includes a Cancel button while a workflow is running (best-effort cancellation).
- The composer stores active workflow waits in server-side client prefs when available (broker/agentd), falling back to localStorage so refresh can resume polling (workflows continue even if the UI reloads).
- If auto-resume cannot reconnect (offline/auth mismatch), the composer shows a Resume/Clear control for persisted waits.
- The workflow summary panel also exposes a Cancel button for `running`/`queued` workflows.
- The workflow summary panel shows the idempotency key when present.
- The workflow summary panel includes copy buttons for workflow id and trace id.
- Recent workflow rows include quick copy buttons for workflow id (plus trace/session/idempotency when present).
- Recent workflow rows show a Cancel button for `running`/`queued` items.
- Recent workflow rows surface a `cancel requested` badge when a stop has been requested.
- Enable the list’s “auto” toggle to refresh recent workflows every few seconds.
- The status filter includes `active` (running + queued) and `all` to list every workflow state; it defaults to `active`.
- Use the list filter box to match workflow id, trace id, session id, or idempotency key (maps to `q` on `/api/v1/workflows`); it debounces typing and includes a clear button. When present, idempotency keys are shown in the workflow list rows.
- Graph mode supports drag-and-drop layout, click-to-connect dependencies, and JSON import/export.
- `agentd_parallel` tasks require `--workflow-enable-http-tasks` on the primary agentd.
- Exporting from Graph mode switches the composer to JSON for review.
- Graph mode only supports LLM and `agentd_parallel` tasks (use JSON for advanced kinds).
- Example `agentd_parallel` template: `docs/examples/workflows/agentd_parallel_demo.json` (paste into JSON tab or import in Graph mode).
- If remote providers are slow, increase `agentd_call.timeout_ms` and adjust `poll_ms`.
  - Generate a live-target demo JSON (uses `out/devstack_state.json` when present):
    - `python3 tools/gen_agentd_parallel_demo.py`
    - `tools/gen_agentd_parallel_demo.sh`
  - Submit the demo workflow to the local agentd (respects `out/devstack_state.json`):
    - `tools/submit_agentd_parallel_demo.sh`
    - `python3 tools/submit_agentd_parallel_demo.py`
  - Add `--wait` to either submit helper to block until the workflow finishes.

## Rendering notes

- The UI renders a **Conversation** (message cards) derived from daemon `events`:
  user prompt → assistant messages → tool calls/results.
- Markdown is rendered with GFM + syntax highlighting for code blocks.
- The settings panel is collapsible and settings persist via `localStorage`.
- If outbound networking requires a proxy, set **HTTPS proxy** in Settings (sent as `proxy` in `POST /api/v1/run`).

## Reliability notes

- The UI persists the active async `job_id` + SSE cursor so refresh can resume a running job stream.
- Team runs in async mode persist member `job_id`s in the broker DB; status lookups remain valid after refresh.
- The Team run panel can persist the last-focused run per team and resume the status lookup after refresh (optional toggle).
- The selected `session_id` is stored per daemon base URL.
- The Scene cache is stored per `session_id`.
- The UI posts acknowledgement events (`ui_action_shown`, `client_rpc_result`,
  `artifact_rendered` / `artifact_render_failed`) for deterministic “Definition of Done” flows
  (see `docs/DOD_ACK.md`).

## Defaults

The WebUI defaults to:
- YOLO enabled
- client RPC enabled
- client RPC side effects enabled
- full tool-call/event visibility in History
- tools=host with host policy=full (daemon default unless overridden)
- automation profile unset (uses daemon defaults; daemon ships with full automation by default)
