# UM‑EAIS v0.3 — Portable Result Hashing (Draft)

Date: 2026-02-05

This document is a **delta** on top of UM‑EAIS v0.2 (`docs/spec/um-eais/um-eais-v0.2.md`).

Goal (v0.3): make `result_sha256` a **portable** and **deterministic** correctness surface across:

- MCU nodes (embedded `agent_core`)
- host nodes
- the platform/broker (`agentd`)

So that:

- node-provided attestation can match platform-computed hashes
- multi-node quorum joins can compare identical work products
- replay/correctness checks remain stable as time advances

Additional goals (v0.3):

- Full PKI / identity / signature verification story (enforced, not best-effort).
- Standardized signing formats and trust roots (targeting v0.4+).

---

## 1) Canonical hash algorithm: `agent_json_c14n_v1`

In v0.3, `result_sha256` is defined as:

- `sha256( canonical_json_bytes(body.result_without_attest) )`
- where `canonical_json_bytes` is UTF‑8 JSON produced by `agent_json_c14n_v1`

Where `body.result_without_attest` is `body.result` with the optional `attest` key removed.

Rationale:
- Nodes may include `result.attest.result_sha256` and (future) `result.attest.sig` metadata.
- If `attest` were included in the hash surface, `result_sha256` becomes self-referential and unstable.

### 1.1 Canonical JSON rules (agent_json_c14n_v1)

Given any valid JSON value:

1. **Objects**
   - Members MUST be emitted in ascending lexicographic order by key (compare key bytes as UTF‑8).
   - Output uses no extra whitespace.
2. **Arrays**
   - Element order is preserved.
3. **Strings**
   - Input strings are parsed/unescaped to code points, then re-escaped deterministically.
   - Output escapes:
     - `"` and `\\`
     - control characters (`< 0x20`) via `\\u00XX`
     - optionally `\\b \\f \\n \\r \\t` for the corresponding control codes
4. **Numbers**
   - Numbers are normalized to a *plain decimal* form (no exponent) by exact base‑10 shift:
     - `2.0000e+0` → `2`
     - `1e-3` → `0.001`
     - `1000e-3` → `1`
   - Trailing fraction zeros are removed; the decimal point is removed if the fraction becomes empty.
   - `-0`, `-0.0`, `-0e+9` normalize to `0` (no sign).

### 1.2 Output token form

`result_sha256` is serialized as:

- `sha256:` + 64 lowercase hex chars

Example canonicalization:

- Input: `{"b":1,"a":2.0000e+0}`
- Canonical: `{"a":2,"b":1}`
- Hash: `sha256(<canonical bytes>)`

---

## 2) Message-level semantics

### 2.1 Node-provided attestation

For `TASK_DONE`, nodes may include:

`body.result.attest.result_sha256 = sha256(canonical_json_bytes(body.result_without_attest))`

Nodes MAY also include an optional hint:

- `body.result.attest.hash_alg = "agent_json_c14n_v1"`

(The platform treats this as best-effort metadata; it does not gate success in v0.3.)

Optional signature (best-effort; platform does not gate success):

If nodes also include:
- `body.result.attest.kid` (id token)
- `body.result.attest.alg` (`ed25519` or `hmac-sha256`)
- `body.result.attest.sig` (base64 signature bytes)
- `body.result.attest.ts_utc_ms` (int64 timestamp)

Then the platform may verify a versioned signing string:

`UM_EAIS_RESULT_ATTEST_v0_1\n<task_id>\n<step_id>\n<idempotency_key>\n<result_sha256>\n<ts_utc_ms>\n`

and emit evidence under task events (visible via `GET /api/v1/trace?trace_id=...`).

Embedded helper: `agent_core` exposes `agent_um_eais_result_attest_signing_input_v0_1(...)` to generate the exact signing bytes.

### 2.2 Platform-computed hash surface

The platform MUST compute and persist:

- `edge_tasks.result_sha256`: sha256 of canonical JSON bytes for the stored `result_json` (with `attest` excluded)

Best-effort robustness:

- If canonicalization fails (invalid JSON, pathological numeric expansion, etc), the platform may fall back to hashing the
  platform-stored `result_json` bytes and should emit evidence under task events:
  - `_platform_result_sha256_alg`
  - `_platform_result_c14n_error`

---

## 3) Compatibility

- v0.1/v0.2 nodes can still interoperate (they can omit `attest`).
- v0.3 nodes can compute `result_sha256` portably and match the platform.
