# Eval Packs

Eval packs run one or more scenarios and apply deterministic checks with scoring.

Run a pack:

```bash
python3 tools/eval_pack.py --file tools/eval_packs/basic_agentd_smoke.json
python3 tools/eval_pack.py --file tools/eval_packs/eval_pack_smoke.json
python3 tools/eval_pack.py --file tools/eval_packs/eval_pack_checks_smoke.json
python3 tools/eval_pack.py --file tools/eval_packs/broker_smoke.json
python3 tools/eval_pack.py --file tools/eval_packs/broker_team_runs_quorum_compose_smoke.json
python3 tools/eval_pack.py --file tools/eval_packs/broker_team_runs_runtime_members_compose_smoke.json
python3 tools/eval_pack.py --file tools/eval_packs/broker_team_runs_role_overrides_compose_smoke.json
python3 tools/eval_pack.py --file tools/eval_packs/broker_team_runtime_members_events_sse_compose_smoke.json
python3 tools/eval_pack.py --file tools/eval_packs/broker_team_quorum_events_sse_compose_smoke.json
python3 tools/eval_pack.py --file tools/eval_packs/broker_team_run_events_sse_compose_smoke.json
```

Outputs are written under `out/eval_pack_<ts>/summary.json`.

Run a canonical pack set:

```bash
tools/run_eval_pack_set.sh --set self-contained
tools/run_eval_pack_set.sh --set canonical
tools/run_eval_pack_set.sh --set live
```

For live-stack packs such as `basic_agentd_smoke.json` and `broker_smoke.json`,
the scenario runner now defaults to the canonical stack from `out/devstack_state.json`
when present. Override with `AGENT_DEVSTACK_STATE=/path/to/devstack_state.json`.

Notes:
- `broker_team_runs_quorum_compose_smoke.json` requires Docker + Docker Compose (it brings up the broker stack if needed).
- `broker_team_runs_runtime_members_compose_smoke.json` requires Docker + Docker Compose (it brings up the broker stack if needed).
- `broker_team_runs_role_overrides_compose_smoke.json` requires Docker + Docker Compose (it brings up the broker stack if needed).
- `broker_team_runtime_members_events_sse_compose_smoke.json` requires Docker + Docker Compose (it brings up the broker stack if needed).
- `broker_team_quorum_events_sse_compose_smoke.json` requires Docker + Docker Compose (it brings up the broker stack if needed).
- `broker_team_run_events_sse_compose_smoke.json` requires Docker + Docker Compose (it brings up the broker stack if needed).
- `broker_smoke.json` and `basic_agentd_smoke.json` are intended for an already-live stack, typically the canonical devstack.

Baseline regression gating:

```bash
tools/run_eval_pack_set.sh --set self-contained --baseline auto
tools/run_eval_pack_set.sh --set canonical --baseline auto
python3 tools/eval_pack.py --file tools/eval_packs/basic_agentd_smoke.json --baseline auto
tools/update_eval_baselines.sh
```

Notes:
- `--baseline auto` resolves repo packs under `tools/eval_packs/` to tracked canonical baselines under `ref/eval_packs/`.
- `--update-baseline` writes a normalized baseline summary without run timestamps or absolute paths, so tracked baselines stay stable in git.
- `tools/run_eval_pack_set.sh --set canonical` runs self-contained packs always, plus live-stack packs when the canonical devstack is available.
- `tools/run_eval_pack_set.sh --set all` requires a live canonical devstack and runs both self-contained and live-stack packs.
- `tools/update_eval_baselines.sh` refreshes the canonical tracked baselines using the same canonical set selection logic.
