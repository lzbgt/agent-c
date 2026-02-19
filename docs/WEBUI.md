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

Build-time overrides (optional): `VITE_AGENTD_BASE_URL`, `VITE_BROKER_BASE_URL`, and `VITE_AGENTUI_*`.

## Broker mode and profiles

- The WebUI can store multiple **connection profiles** (direct or broker-backed) in localStorage.
- Each profile can enable **profile-specific run settings** (model/provider, tool flags, run limits).
- The broker console exposes agent membership management and audit events.

## Diagnostics

Settings → Diagnostics triggers:
- `/api/v1/diagnostics`
- `/api/v1/diagnostics/providers`
- `/api/v1/diagnostics/provider_test`

See `docs/DIAGNOSTICS.md` for provider_test usage.

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
