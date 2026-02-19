# Eval Packs

Eval packs run one or more scenarios and apply deterministic checks with scoring.

Run a pack:

```bash
python3 tools/eval_pack.py --file tools/eval_packs/basic_agentd_smoke.json
python3 tools/eval_pack.py --file tools/eval_packs/eval_pack_smoke.json
python3 tools/eval_pack.py --file tools/eval_packs/eval_pack_checks_smoke.json
python3 tools/eval_pack.py --file tools/eval_packs/broker_smoke.json
python3 tools/eval_pack.py --file tools/eval_packs/broker_team_runs_quorum_compose_smoke.json
```

Outputs are written under `out/eval_pack_<ts>/summary.json`.

Notes:
- `broker_team_runs_quorum_compose_smoke.json` requires Docker + Docker Compose (it brings up the broker stack if needed).
