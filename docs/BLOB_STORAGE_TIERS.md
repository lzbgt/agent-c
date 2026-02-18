# Blob Storage Tiers (Design + Status)

Date: 2026-02-18

This document defines a future-proof, multi-tier blob storage plan for agentd. The DB remains a metadata index; binary
blobs live in tiered stores with explicit retention, budgets, and deterministic references.

Status (2026-02-18):
- v0 local tier shipped.
- v1 object-store tier shipped (S3/MinIO; presigned reads + proxy mode; optional read-through cache).
- v2 tiering policy engine (promote/evict) is operator-driven via `/api/v1/blob/tier/enforce`.

## Goals

- Support binary artifacts (images/audio/video) and large tool outputs without inflating the SQLite DB.
- Provide deterministic blob identity (hash + size) so evidence bundles and replay workflows are stable.
- Allow multiple storage tiers with policy-driven promotion/eviction (local hot cache → object store → archive).
- Keep reads and streaming safe: range support, size limits, auth, and access logging.
- Preserve operability: simple local-only mode for dev, object-store mode for production.

## Constraints

- DB remains a metadata index, not a binary store.
- Storage must be safe under concurrent writes and crash recovery.
- Security: blobs are always auth-gated; external URLs must be signed and short-lived.
- Retention policies must be explicit and auditable.

## Proposed data model

### Blob identity

- `blob_id`: `sha256:<hex>` (content-addressed).
- `size_bytes`: integer.
- `mime`: best-effort MIME type.
- `created_utc_ms`: creation time.

### Suggested DB tables

`blob_manifest` (new):
- `blob_id TEXT PRIMARY KEY`
- `size_bytes INTEGER NOT NULL`
- `mime TEXT`
- `sha256_hex TEXT NOT NULL` (redundant but explicit)
- `created_utc_ms INTEGER NOT NULL`
- `last_access_utc_ms INTEGER`
- `ref_count INTEGER NOT NULL DEFAULT 0`
- `tier TEXT NOT NULL` (`local` | `object` | `archive`)
- `location TEXT NOT NULL` (local path or object key)
- `etag TEXT` (object store)
- `storage_class TEXT` (object store class)

`artifact_blobs` (new join):
- `artifact_id INTEGER NOT NULL`
- `blob_id TEXT NOT NULL`
- `PRIMARY KEY(artifact_id, blob_id)`

Artifacts retain `path` for legacy compatibility, but new records should prefer `blob_id`.

## Tier layout

### Tier 1: Local file store (default)

- Root: `${state_dir}/blobs/sha256/<aa>/<bb>/<sha256>`.
- Atomic writes: write temp + fsync + rename.
- Safe delete: GC by ref_count and retention policy.
- Upload size cap: enforced via the daemon `upload_max_bytes` limit in v0.

### Tier 2: Object store (S3/MinIO)

- Key: `blobs/sha256/<aa>/<bb>/<sha256>` (prefix configurable).
- Presigned URLs for clients; agentd can proxy for auth-only environments.
- Optional local cache on read-through (bounded by size).

### Tier 3: Archive

- Optional cold storage for compliance.
- Read-through may be async with restore delay; surfacing a `restore_pending` state.

## Write path

1) Tool emits artifact → blob bytes stored in local tier (unless object-store mode with cache disabled).
2) Compute hash + size, register in `blob_manifest` (`tier`, `location`, `etag`).
3) Create `artifact_blobs` association.
4) Background mover (future) enforces policies:
   - promote to object store
   - evict local based on LRU, size cap, or TTL

## Read path

- Resolve `blob_id` → tier + location.
- If local: stream with `Range` support.
- If object store:
  - `read_mode=redirect` returns `302` to a signed URL.
  - `read_mode=proxy` streams via agentd.
- Cache on read when `cache_mode=read-through` and size <= `cache_max_bytes`.

## APIs (current)

Read:
- `GET /api/v1/blob?blob_id=...` (supports `Range`)
  - local tier: returns bytes
  - object tier: `302` redirect (default) or proxy stream (see `read_mode`)

Upload:
- `POST /api/v1/blob/upload` (JSON `data_base64` or raw binary)
  - response includes `tier`, `location`, `etag`, and `storage_class` when object-store is enabled

Metadata:
- `GET /api/v1/blob/meta?blob_id=...`
- `POST /api/v1/blob/retain` (adjust ref count; `{blob_id, delta}`)
- `POST /api/v1/blob/gc` (ref-count GC sweep; `{min_age_ms, max_rows, dry_run}`)
- `POST /api/v1/blob/tier/enforce` (apply tiering policy once; safe maintenance endpoint)

Archive controls (v3, operator-only):
- `POST /api/v1/blob/archive` with `{ blob_id | blob_ids, storage_class? }` to mark blobs as `tier="archive"`.
- `POST /api/v1/blob/restore` with `{ blob_id | blob_ids, storage_class?, clear_storage_class? }` to return to `tier="object"`.

Notes:
- Archive/restore requires `blob_store.mode=object` and configured object-store credentials.
- This is **metadata-driven**; agentd blocks reads for `tier="archive"` until restored.
- Cold storage transitions are expected to be managed by object-store lifecycle policies or external tooling.

These are additive; legacy `GET /api/v1/file` remains supported.

## Retention + GC policy

- Policies by tier, size, MIME, and age.
- Ref-counted GC: remove only when `ref_count == 0` and TTL expired.
- Optional "pin" labels for evidence bundles.

## Observability

- Counters: bytes per tier, objects count, cache hit/miss, promotion/eviction counts.
- Auditable events for GC and tier transitions.
- DB query endpoints: `/api/v1/db/blobs`, `/api/v1/db/blob`, `/api/v1/db/analytics/blobs`.

## Object-store configuration (v1)

Runtime config (JSON / `POST /api/v1/config/update`):
- `blob_store`:
  - `mode`: `local` or `object`
  - `endpoint`: `https://s3.us-east-1.amazonaws.com` or `http://localhost:9000`
  - `region`: AWS region (default `us-east-1`)
  - `bucket`: bucket name
  - `prefix`: object key prefix (default `blobs/sha256`)
  - `path_style`: `true` for path-style bucket addressing (MinIO-friendly)
  - `read_mode`: `redirect` (presigned URL) or `proxy` (agentd stream)
  - `cache_mode`: `read-through` or `none`
  - `cache_max_bytes`: max size to read-through cache (bytes)
  - `presign_ttl_sec`: TTL for signed URLs (seconds, 1..604800)
  - `timeout_ms`: object store timeout
- `blob_store_secrets`:
  - `access_key`, `secret_key`, `session_token` (optional)

Env vars (startup defaults):
- `AGENTD_BLOB_STORE_MODE`, `AGENTD_BLOB_STORE_ENDPOINT`, `AGENTD_BLOB_STORE_REGION`,
  `AGENTD_BLOB_STORE_BUCKET`, `AGENTD_BLOB_STORE_PREFIX`, `AGENTD_BLOB_STORE_PATH_STYLE`,
  `AGENTD_BLOB_STORE_READ_MODE`, `AGENTD_BLOB_STORE_CACHE_MODE`, `AGENTD_BLOB_STORE_CACHE_MAX_BYTES`,
  `AGENTD_BLOB_STORE_PRESIGN_TTL_SEC`, `AGENTD_BLOB_STORE_TIMEOUT_MS`,
  `AGENTD_BLOB_STORE_ACCESS_KEY`, `AGENTD_BLOB_STORE_SECRET_KEY`, `AGENTD_BLOB_STORE_SESSION_TOKEN`.

## Tiering policy engine (v2)

The tiering policy engine is **explicit** and **operator-driven**: it runs only when invoked
via `/api/v1/blob/tier/enforce` (or an operator cron), making it deterministic and easy to audit.

Policies (config defaults):
- `blob_tier.local_max_bytes`: cap total local cache bytes for **object-tier** blobs (0 disables).
- `blob_tier.local_max_age_ms`: evict object-tier local cache older than this age (0 disables).
- `blob_tier.promote_after_ms`: promote **local-tier** blobs to object store when older than this age (0 disables).
- `blob_tier.promote_max_bytes`: per-blob size cap for promotion (0 disables).

Notes:
- Local eviction never deletes **local-tier** blobs (avoids data loss).
- Promotion requires object-store configuration and `blob_store.mode=object`.
- `cache_mode=none` forces eviction of any object-tier local cache discovered.

Endpoint:
`POST /api/v1/blob/tier/enforce` with optional overrides:
```json
{
  "dry_run": false,
  "local_max_bytes": 1073741824,
  "local_max_age_ms": 604800000,
  "promote_after_ms": 86400000,
  "promote_max_bytes": 33554432,
  "max_rows": 5000
}
```

Response (JSON):
- `ok` (boolean)
- `generated_utc_ms` (number)
- `promoted_count`, `promoted_bytes`
- `evicted_count`, `evicted_bytes`
- `total_local_bytes_before`, `total_local_bytes_after`
- `errors` (array, optional)

## Phased delivery

1) **v0 (local-only)**: `blob_manifest` + local store + read endpoint + ref-count GC. (shipped)
2) **v1 (object store)**: S3/MinIO backend + signed URLs + read-through cache. (shipped)
3) **v2 (tiering)**: policy engine + background mover + size budgets.
4) **v3 (archive)**: cold storage restore workflow + operator controls.

## Compatibility

- Existing artifacts with `path` remain valid.
- New artifact JSON should include `blob_id` when available.
