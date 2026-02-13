# Run Replay Bundles (Deterministic Audit Surface)

Date: 2026-02-14

This document defines the **run replay bundle** feature for `agentd`.

## Goals

- Provide a **deterministic replay surface** for each run (request + response + tool timeline).
- Enable **offline verification** by hashing a canonical JSON bundle.
- Keep storage bounded and **safe for secrets** (redaction rules).

## Additional goals

- Improve reproducibility across providers with provider snapshots, deterministic defaults, and replay annotations.
- Support large binary blobs with size limits and tiered storage, while keeping file-backed fallbacks.

## What is a replay bundle?

A replay bundle is a **redacted, deterministic snapshot** of a run:

- **request**: the run request JSON (redacted)
- **response**: the daemon response JSON (redacted)
- **tool_records**: per-tool call inputs/outputs (already stored in DB)
- **hash**: SHA256 token over a canonical JSON bundle (`agent_json_c14n_v1`)

The bundle is stored with the run row (SQLite) and can be retrieved via an API endpoint.

## Redaction rules (request)

The replay request is **redacted** before storage:

- Remove: `api_key`, `Authorization`, `auth_token`, `trace_text`, `http_body`
- Strip any `input_files[].data_base64` if present
- Keep the rest of the request intact

If the redacted JSON exceeds size limits, it is **not stored**.

## Redaction rules (response)

The replay response is **redacted** before storage:

- Remove: `http_body`, `trace_text`

If the redacted JSON exceeds size limits, it is **not stored**.

## Hashing

The daemon computes:

- `replay_sha256`: `sha256:` token over a canonical JSON bundle
- `replay_sha256_alg`: `agent_json_c14n_v1`
- `replay_sha256_schema`: `run_replay_bundle_v1`

Canonicalization uses `agent_json_c14n_v1` to normalize keys, numbers, and whitespace.

## Storage bounds

Replay storage is best-effort and bounded:

- Request JSON max: **512 KiB** (redacted)
- Response JSON max: **1 MiB** (redacted)

If a payload exceeds the cap, it is omitted and `replay_error` is recorded.

## Endpoint

`GET /api/v1/run/replay?run_id=<id>`

Response (200):

```json
{
  "ok": true,
  "run_id": 123,
  "bundle": {
    "schema": "run_replay_bundle_v1",
    "request": { ... },
    "response": { ... },
    "tool_records": [ ... ]
  },
  "replay_sha256": "sha256:...",
  "replay_sha256_alg": "agent_json_c14n_v1",
  "replay_sha256_schema": "run_replay_bundle_v1",
  "replay_error": ""
}
```

If replay data is unavailable (e.g., oversized or disabled by `no_session`), the endpoint
returns `ok=false` with an error message and HTTP 404 or 409.

## Notes

- Runs with `no_session=true` do **not** persist replay bundles.
- Tool records are returned in the bundle for full traceability.
