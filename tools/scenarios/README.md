# Scenario runner (agentd + broker)

This folder contains **data-driven scenarios** for agentd/broker bring-up.
Each scenario is a JSON file executed by `tools/scenario_runner.py`.

## Run a scenario

```bash
python3 tools/scenario_runner.py --file tools/scenarios/agentd_smoke.json
```

Output directory is printed on success (default: `out/scenario_<ts>/`).
Logs are written under `out/scenario_<ts>/logs/`.

## Scenario format

```json
{
  "name": "agentd_smoke",
  "steps": [
    { "type": "shell", "cmd": "curl -fsS http://127.0.0.1:8123/api/v1/health" },
    { "type": "capture_evidence", "args": ["--agentd-base", "http://127.0.0.1:8123"] }
  ]
}
```

Supported step types:
- `shell`: run a shell command. Fields: `cmd`, optional `cwd`, `env`, `timeout_s`, `allow_failure`.
- `http`: simple HTTP request (GET/POST). Fields: `url`, `method`, `headers`, `body`, `expect_status`, `save_as`, `insecure`, `timeout_s`.
- `sleep`: pause. Fields: `seconds` or `duration_s`.
- `capture_evidence`: run `tools/capture_agent_evidence_bundle.sh` with `args` list.
- `set`: set a template variable for later steps. Fields: `key`, `value`.

Template variables:
- `{{run_dir}}` (scenario output directory)
- `{{scenario}}` (scenario name)
- `{{evidence_dir}}` (last evidence bundle dir)
- `{{env.VAR}}` (environment variable)

## Included scenarios

- `agentd_smoke.json`: checks `/api/v1/health` + `/api/v1/diagnostics`, captures evidence.
- `broker_smoke.json`: checks `/healthz` + `/readyz` (TLS insecure), captures evidence via broker proxy.

## Scenario pack

Run all scenarios in this folder and validate evidence bundles:

```bash
python3 tools/scenario_pack.py --dir tools/scenarios --validate
```
