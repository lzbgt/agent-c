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
- `connectionMode` (`direct` or `broker`)
- `daemonBaseUrl`, `brokerBaseUrl`, `brokerAgentId`, `brokerDeploymentId`
- `daemonAuthToken`, `brokerAuthToken` (only if you accept putting tokens in a static file)
- `model`, `baseUrl`, `proxyUrl`, `timeoutMs`
- `tools`, `yolo`, `hostPolicy`, `verbose`
- `allowClientRpcs`, `allowClientEffects`, `allowUnsafePageEval`
- `workflowAgentTargets` (array or CSV of agentd base URLs for collaboration templates)
- `workflowBearerEnv` (env var name used by `agentd_call` bearer_env in templates)

Build-time overrides (optional): `VITE_AGENTD_BASE_URL`, `VITE_BROKER_BASE_URL`, and `VITE_AGENTUI_*`.

## Broker mode and profiles

- The WebUI can store multiple **connection profiles** (direct or broker-backed) in localStorage.
- Each profile can enable **profile-specific run settings** (model/provider, tool flags, run limits).
- The broker console exposes agent membership management, audit events, and team quorum approvals.

## Diagnostics

Settings → Diagnostics triggers:
- `/api/v1/diagnostics`
- `/api/v1/diagnostics/providers`
- `/api/v1/diagnostics/provider_test`

See `docs/DIAGNOSTICS.md` for provider_test usage.

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
- The workflow summary panel also exposes a Cancel button for `running`/`queued` workflows.
- Recent workflow rows show a Cancel button for `running`/`queued` items.
- Recent workflow rows surface a `cancel requested` badge when a stop has been requested.
- Enable the list’s “auto” toggle to refresh recent workflows every few seconds.
- The status filter includes `active` (running + queued) and `all` to list every workflow state; it defaults to `active`.
- Use the list filter box to match workflow id, trace id, or session id.
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
