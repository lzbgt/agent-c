# Borrowed Workflow Ideas (from urine_monitor)

This document captures **generic workflow facilities** observed in `../urine_monitor` that are
high-leverage for this repo (agentd + broker + WebUI). It is not a spec; it is a prioritized
idea list with concrete adoption hooks.

## 1) Evidence bundles (capture + validate)

Urine monitor uses a repeatable **evidence bundle** workflow:
- capture authoritative server snapshots
- include optional admin history
- validate bundles via a machine-checkable script

Adoption here:
- New tool: `tools/capture_agent_evidence_bundle.sh`
  - captures agentd health/config/diagnostics + optional broker state
  - supports calling agentd via broker proxy (`--agentd-via-broker`)
  - optional trace capture (`--trace-id`)
- Validator: `tools/check_agent_evidence_bundle.py` (checks JSON parse + ok fields, strict mode).

## 2) Data-driven scenario packs

Urine monitor’s SIL scenarios are JSON-driven and produce evidence bundles.
They enable fast regression without hardware.

Adoption here:
- Added `tools/scenarios/` for agentd/broker workflows (run requests, trace capture, audit checks).
- Added `tools/scenario_runner.py` to execute JSON scenarios and capture evidence bundles.

## 3) One-command dev stack

Urine monitor has a “devstack” orchestrator to make the platform usable without hardware.

Adoption here (proposed):
- Provide a single script that:
  - starts agentd + broker + webui (optional)
  - performs smoke checks (health + diagnostics + basic run)
  - outputs a trace id and evidence bundle for reproducibility

## 4) API error envelope

Urine monitor defines a consistent JSON error envelope for `/api/v1/*` endpoints.
It improves scripts + UI error handling.

Adoption here (proposed):
- Align agentd + broker error responses on a stable JSON shape:
  - `{"err":"...","code":"...","details":{...}}`
- Update WebUI + tooling to surface `code` and `details` when present.

## 5) Operator-ready UX defaults

Urine monitor’s WebUI is embedded and assumes admin/auth input via local storage,
with tooling that works even in dev contexts.

Adoption here:
- WebUI broker console (agent list + membership mgmt + audit) added.
- Continue to push runtime config via `agentui-config.js` to avoid rebuilds.

## References (source of ideas)

Key urine_monitor sources reviewed:
- `docs/spec/platform_evidence_bundle.md`
- `tools/scenarios/README.md`
- `docs/stabilization/S0_sil_devstack.md`
- `docs/spec/platform_api_error_envelope.md`
