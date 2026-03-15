# Run Diff + Evidence Comparison v0

Date: 2026-02-20
Status: v0.3 (client-side replay + DB evidence + attestation diff UX shipped; server-side persisted diff endpoints still planned)

## Summary

Provide a deterministic way to compare two runs (or a run vs a baseline)
using evidence bundles as the source of truth. The diff output is
machine-checkable and UI-friendly.

## Current implementation (v0.2)

- WebUI uses **replay bundles** from `GET /api/v1/run/replay?run_id=...` and computes
  a client-side diff across request/response/tool records.
- WebUI can optionally load **DB-backed evidence** (`/api/v1/db/run`) to diff events and artifacts.
- WebUI surfaces **attestation bundles** (`/api/v1/run/attestation`) as signed evidence metadata.
- The diff output is UI-facing only (not persisted in agentd yet).
- Baselines are stored in the browser (per base URL) for quick reuse.
- Direct browser regression coverage exists in `ui/e2e/run_diff_panel.spec.ts`.

## Goals

- Deterministic diffs across runs, including events, artifacts, and costs.
- Evidence-first: diffs are derived from captured evidence bundles.
- Stable ordering and hashing so diffs can be used for CI regression gates.
- UI-ready structure for side-by-side comparison.

## Non-goals

- Re-running or re-evaluating prompts during diff.
- Model-level semantic comparison beyond evidence outputs.
- Interactive edits to runs (handled by replay workflows).

## Inputs

1) Replay bundle A (`/api/v1/run/replay`)
2) Replay bundle B (`/api/v1/run/replay`)
3) Optional DB run evidence (`/api/v1/db/run`)
4) Optional attestation bundle (`/api/v1/run/attestation`)

Replay bundles include:
- run metadata (model, provider, limits, timestamps)
- event log (run/workflow events with stable indices)
- tool IO hashes
- artifact inventory (names, sizes, hashes)
- cost/usage summary

## Output schema (diff result)

Top-level fields:
- `diff_id` (stable hash of bundle ids + version)
- `lhs` and `rhs` bundle ids
- `status` (same | different | error)
- `summary` (counts of changed items)
- `events` (per-event diffs, ordered by index)
- `artifacts` (added/removed/changed lists)
- `usage` (cost + token deltas)
- `checks` (optional policy or eval failures)

## Deterministic ordering

- Events are ordered by `(stream_id, event_index)` from the bundle.
- Artifacts are ordered by `path` then `sha256`.
- Usage deltas are ordered by provider, then model.

## API surface (agentd)

### Implemented today

- `GET /api/v1/run/replay?run_id=...`
  - returns: replay bundle JSON (request + response + tool records + hashes)
- `GET /api/v1/db/run?run_id=...&include_events=1&include_artifacts=1`
  - returns: run row + events + artifacts from SQLite (when enabled)
- `GET /api/v1/run/attestation?run_id=...`
  - returns: signed attestation bundle (hash metadata + signature)

### Planned

- `POST /api/v1/run/diff`
  - body: `{ "lhs_bundle_id": "...", "rhs_bundle_id": "...", "options": {...} }`
  - returns: diff result JSON

- `GET /api/v1/run/diff/{diff_id}`
  - returns previously computed diff (if persisted)

## UI expectations

- Side-by-side timeline with event-level diffs (planned).
- Artifact diff view with hash changes and sizes (planned).
- Cost/usage deltas highlighted.
- Evidence bundle links preserved.

## Evidence tests

- Fixture bundles with known diffs (events + artifacts).
- Hash stability check: same input bundles produce identical diff.
- CI gate: diff must be generated with `status=done` and no schema errors.
- Current shipped UI regression: `ui/e2e/run_diff_panel.spec.ts`

## References

- `docs/spec/run_attestation_bundle_v1.md`
- `docs/spec/run-events/run_events_v1.md`
- `docs/AGENTIC_VISION.md`
