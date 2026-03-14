# Eval Pack Spec v0 (draft)

Status: draft (rolling)
Date: 2026-02-19

This spec defines a lightweight, deterministic **evaluation pack** format for regression gating.
Eval packs run one or more scenarios and apply structured checks with scoring and thresholds.

## Goals

- Deterministic pass/fail checks (no subjective grading).
- Reproducible scoring across runs.
- Minimal dependencies (JSON + shell + existing scenario runner).
- CI-friendly summary output.

## Non-goals

- Automated LLM grading or subjective scoring.
- Full JSONPath implementation (v0 uses simple dotted paths).
- Provider-specific benchmarking (handled by scenario inputs).

---

## 1) Pack format (JSON)

Top-level object:

```json
{
  "name": "basic_agentd_smoke",
  "version": "eval_pack_v0",
  "env": { "KEY": "VALUE" },
  "threshold": { "min_score": 1.0, "min_pass_rate": 1.0 },
  "scenarios": [ { "...": "..." } ]
}
```

Fields:

- `name` (string, optional): human-friendly pack name.
- `version` (string): `eval_pack_v0` for this spec.
- `env` (object, optional): environment variables applied to every scenario run.
- `threshold` (object, optional):
  - `min_score` (number, default `0.0`): minimum total score to pass.
  - `min_pass_rate` (number, default `0.0`): required pass rate (0–1).
- `scenarios` (array, required): list of scenario entries.

---

## 2) Scenario entries

```json
{
  "id": "agentd_smoke",
  "file": "tools/scenarios/agentd_smoke.json",
  "out_dir": "agentd_smoke",
  "score_weight": 1.0,
  "env": { "KEY": "VALUE" },
  "checks": [ { "...": "..." } ]
}
```

Fields:

- `id` (string): scenario identifier (used in summaries).
- `file` (string, required): scenario JSON file consumed by `tools/scenario_runner.py`.
- `file` resolution: relative paths are resolved against the pack file directory; if missing, the repo root is used as a fallback.
- `out_dir` (string, optional): output directory name under the pack output root.
- `score_weight` (number, default `1.0`): score contribution when checks pass.
- `env` (object, optional): scenario-specific environment variables.
- `checks` (array, required): list of deterministic checks.

---

## 3) Check types (v0)

### 3.1 `file_exists`

```json
{ "type": "file_exists", "path": "meta.json" }
```

Asserts that a file exists in the scenario `run_dir`.

### 3.2 `file_sha256`

```json
{ "type": "file_sha256", "path": "hello.txt", "sha256": "..." }
```

Asserts that a file’s SHA256 matches the provided hex digest.

### 3.3 `log_contains`

```json
{ "type": "log_contains", "path": "logs/01_agentd_health.log", "contains": "ok" }
```

Checks that a log file contains a substring. Use `pattern` for regex:

```json
{ "type": "log_contains", "path": "logs/01_agentd_health.log", "pattern": "status: 200" }
```

### 3.4 `json_path`

```json
{ "type": "json_path", "path": "meta.json", "key": "scenario", "equals": "agentd_smoke" }
```

`key` is a dot-separated path with optional integer indices (e.g., `results[0].ok`).
If `equals` is omitted, the check only asserts the path exists.

### 3.5 `json_number`

```json
{ "type": "json_number", "path": "meta.json", "key": "metrics.score", "min": 0, "max": 1 }
```

Validates a numeric JSON value. Supports `equals`, `min`, and `max`.

### 3.6 `json_len`

```json
{ "type": "json_len", "path": "meta.json", "key": "results", "equals": 3 }
```

Validates the length of a JSON array/object/string. Supports `equals`, `min`, and `max`.

---

## 4) Scoring + thresholds

- Each scenario contributes `score_weight` if **all** checks pass.
- `total_score` is the sum of passing scenario weights.
- `pass_rate` is `passing_scenarios / total_scenarios`.
- The pack passes if:
  - `total_score >= min_score`, and
  - `pass_rate >= min_pass_rate`, and
  - no scenarios failed to execute.

---

## 5) Output summary

`tools/eval_pack.py` writes `summary.json`:

```json
{
  "name": "...",
  "version": "eval_pack_v0",
  "started_at": "2026-02-19T00:00:00Z",
  "finished_at": "2026-02-19T00:02:00Z",
  "results": [ { "id": "...", "ok": true, "score": 1.0, "checks": [...] } ],
  "total_score": 1.0,
  "max_score": 1.0,
  "pass_rate": 1.0,
  "threshold": { "min_score": 1.0, "min_pass_rate": 1.0 },
  "failed": [],
  "ok": true
}
```

---

## 6) Reference tooling

- Runner: `tools/eval_pack.py`
- Example pack: `tools/eval_packs/basic_agentd_smoke.json`
- Scenarios: `tools/scenarios/`

CLI options:
- `--baseline <path|auto>`: compare current summary against a saved baseline summary. `auto` resolves repo packs in `tools/eval_packs/` to `ref/eval_packs/<pack>.summary.json`.
- `--update-baseline`: write a normalized baseline summary to the baseline path. If `--baseline` is omitted, the canonical `auto` path is used.
- Repo scenarios may also consume a canonical live stack from `out/devstack_state.json` via `AGENT_DEVSTACK_STATE`, which keeps live-stack eval packs deterministic about which agentd/broker deployment they exercise.

Example:

```bash
python3 tools/eval_pack.py --file tools/eval_packs/eval_pack_smoke.json --baseline auto
python3 tools/eval_pack.py --file tools/eval_packs/eval_pack_smoke.json --update-baseline
python3 tools/eval_pack.py --file tools/eval_packs/basic_agentd_smoke.json --baseline ref/eval_packs/basic_agentd_smoke.summary.json
python3 tools/eval_pack.py --file tools/eval_packs/basic_agentd_smoke.json --baseline ref/eval_packs/basic_agentd_smoke.summary.json --update-baseline
```

Tracked baselines intentionally omit per-run timestamps and absolute paths so baseline diffs stay reviewable and deterministic in git.
