# Eval Packs

Eval packs run one or more scenarios and apply deterministic checks with scoring.

Run a pack:

```bash
python3 tools/eval_pack.py --file tools/eval_packs/basic_agentd_smoke.json
```

Outputs are written under `out/eval_pack_<ts>/summary.json`.
