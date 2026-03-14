# Scenario runner (agentd + broker)

This folder contains **data-driven scenarios** for agentd/broker bring-up.
Each scenario is a JSON file executed by `tools/scenario_runner.py`.

## Run a scenario

```bash
python3 tools/scenario_runner.py --file tools/scenarios/agentd_smoke.json
```

Output directory is printed on success (default: `out/scenario_<ts>/`).
Logs are written under `out/scenario_<ts>/logs/`.

By default, `scenario_runner.py` reads `out/devstack_state.json` when present and
uses that canonical live stack for `{{env.AGENTD_BASE}}`, `{{env.BROKER_BASE}}`,
`{{env.AGENTD_PUBLISHED_PORT}}`, and `{{env.BROKER_PUBLISHED_PORT}}`.
Override the state path with `AGENT_DEVSTACK_STATE=/path/to/devstack_state.json`.
When a devstack state file is present, `{{env.AGENTD_AUTH_TOKEN}}` defaults to
`dev-agentd-token` unless you override `AGENTD_AUTH_TOKEN` explicitly.

## Scenario format

```json
{
  "name": "agentd_smoke",
  "steps": [
    { "type": "http", "url": "http://127.0.0.1:8123/api/v1/health", "expect_status": 200 },
    { "type": "capture_evidence", "args": ["--agentd-base", "http://127.0.0.1:8123"] }
  ]
}
```

Supported step types:
- `shell`: run a shell command. Fields: `cmd`, optional `cwd`, `env`, `timeout_s`, `allow_failure`.
  - If `timeout_s` triggers, the step log records a timeout error.
- `http`: simple HTTP request (GET/POST). Fields: `url`, `method`, `headers`, `body`, `expect_status`, `save_as`, `insecure`, `timeout_s`, `proxy`.
  - `timeout_s` accepts seconds as float; `<= 0` or empty disables the timeout.
  - Request logs redact common auth headers (Authorization, API keys).
  - Step logs include `allow_failure` and `proxy` when set.
  - `expect_status` may be a single code or list; non-2xx is allowed when it matches (response body is still logged/saved).
- `sleep`: pause. Fields: `seconds` or `duration_s`.
- `capture_evidence`: run `tools/capture_agent_evidence_bundle.sh` with `args` list.
- `set`: set a template variable for later steps. Fields: `key`, `value`.

Template variables:
- `{{run_dir}}` (scenario output directory)
- `{{scenario}}` (scenario name)
- `{{evidence_dir}}` (last evidence bundle dir)
- `{{env.VAR}}` (environment variable)
- Built-in endpoint defaults:
  - `{{env.AGENTD_BASE}}`
  - `{{env.BROKER_BASE}}`
  - `{{env.WEBUI_BASE}}`
  - `{{env.AGENTD_PUBLISHED_PORT}}`
  - `{{env.BROKER_PUBLISHED_PORT}}`

HTTP proxy behavior:
- By default, `http` steps honor standard proxy env vars, but bypass proxies for localhost/127.0.0.1.
- Set `proxy` to `false`/`disabled` to force no proxy, or `true`/`env` to force proxy usage (overrides localhost bypass).

## Included scenarios

- `agentd_smoke.json`: checks `/api/v1/health` + `/api/v1/diagnostics`, captures evidence.
- `broker_smoke.json`: checks `/healthz` + `/readyz` (TLS insecure), captures evidence via broker proxy.
- `broker_proxy_agentd_smoke.json`: validates broker OIDC auth plus `X-Agentd-Authorization` proxy/session access against the canonical live stack.
- `broker_team_runs_quorum_compose_smoke.json`: runs the broker team-run quorum compose smoke (requires Docker Compose).
- `broker_team_runs_runtime_members_compose_smoke.json`: runs the broker team-run runtime members compose smoke (requires Docker Compose).
- `broker_team_runs_role_overrides_compose_smoke.json`: runs the broker team-run role overrides compose smoke (requires Docker Compose).
- `broker_team_runtime_members_events_sse_compose_smoke.json`: validates runtime members SSE events (requires Docker Compose).
- `broker_team_quorum_events_sse_compose_smoke.json`: validates quorum SSE events (requires Docker Compose).
- `broker_team_run_events_sse_compose_smoke.json`: validates team run created/status SSE events (requires Docker Compose).
- `eval_pack_smoke.json`: self-contained eval-pack smoke scenario.
- `eval_pack_checks_smoke.json`: self-contained scenario to exercise eval checks.

## Scenario pack

Run all scenarios in this folder and validate evidence bundles:

```bash
python3 tools/scenario_pack.py --dir tools/scenarios --validate
```

`scenario_pack.py` writes a `summary.json` with per-scenario results, pack-level timing (`started_at`, `finished_at`,
`duration_s`), and counts (`total`, `ok_count`, `failed_count`, `ok`).

During execution, the pack prints per-scenario progress lines with durations (e.g. `[scenario_pack] ok <name> (1.23s)`).
