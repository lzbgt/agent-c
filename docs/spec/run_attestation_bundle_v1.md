# Run Attestation Bundle v1 (Draft)

Date: 2026-02-19

## Goal

Provide a **signed, portable reference** to a run replay bundle without re-embedding the full bundle.
The attestation bundle references the replay bundle hash, includes metadata (run/session/agent/deployment),
and optionally includes a signature over the attestation payload.

This format is intentionally small so it can be used in CI pipelines, audits, and external registries.

## Format (JSON)

Top-level object:

```
{
  "schema": "run_attestation_bundle_v1",
  "created_utc_ms": 1700000000000,
  "replay_sha256": "sha256:...",
  "replay_sha256_alg": "agent_json_c14n_v1",
  "replay_sha256_schema": "run_replay_bundle_v1",

  "run_id": "run_...",               // optional
  "session_id": "sess_...",          // optional
  "agent_id": "agent_...",           // optional
  "deployment_id": "prod-west-1",    // optional

  "issuer": {                         // optional
    "id": "operator@example.com"
  },

  "attest": {                         // optional (signature block)
    "alg": "hmac-sha256",             // or "ed25519"
    "kid": "key-id-1",
    "sig": "<base64>",
    "ts_utc_ms": 1700000000000,
    "hash_alg": "agent_json_c14n_v1",
    "signing_schema": "run_attestation_bundle_v1"
  }
}
```

### Required fields

- `schema`
- `created_utc_ms`
- `replay_sha256`
- `replay_sha256_alg`
- `replay_sha256_schema`

### Replay hash source

The `replay_sha256` token should match the run replay bundle returned by:

- `GET /api/v1/run/replay?run_id=<id>`
- `GET /api/v1/run/attestation?run_id=<id>` returns an unsigned attestation bundle referencing the replay hash.

Server-side signing (optional):

- If `AGENTD_RUN_ATTEST_HMAC_KID` and `AGENTD_RUN_ATTEST_HMAC_KEY` are set, the daemon includes an `attest` block
  on `/api/v1/run/attestation` responses using HMAC-SHA256 over the canonical JSON payload (without `attest`).

See: `docs/WORKFLOWS.md` (Run replay bundles).

## Signing input

When `attest` is present, the signature **MUST** be computed over the canonical JSON
bytes (`agent_json_c14n_v1`) of the attestation bundle **with the `attest` field removed**.

- Canonicalization algorithm: `agent_json_c14n_v1`
- Input is UTF‑8 JSON (no trailing newline)

This mirrors the envelope-signing approach used in UM‑EAIS / UMBMP specs.

## Verification

1) Fetch or load the referenced replay bundle.
2) Verify `replay_sha256` matches the canonical hash of that bundle.
3) If `attest` is present, verify the signature over the attestation bundle
   (with `attest` removed) using the specified algorithm + key.

## Tooling

A host tool is provided to generate the bundle and optional HMAC signature:

```
run_attestation_bundle_tool --replay-json <path> --kid <id> --hmac-key-hex <hex>
```

Ed25519 signing is also supported:

```
run_attestation_bundle_tool --replay-json <path> --kid <id> --ed25519-seed-hex <64hex>
```

The same tool can verify signatures and replay hashes:

```
run_attestation_bundle_tool --verify --attestation-json <path> --replay-json <replay.json> --hmac-key-hex <hex>
```

Convenience wrapper:

```
tools/verify_attestation_bundle.sh <attestation_json> <replay_json> --hmac-key-hex <hex>
```

See: `tools/run_attestation_bundle_tool.cpp`.

## Notes

- The attestation bundle is intentionally small and **does not embed** the replay bundle.
- Future versions can add attestation of tool/plugin manifests or policy VM outputs without
  changing the replay hash surface.
