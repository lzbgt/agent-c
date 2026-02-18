# Blob Storage Tiers (Design)

Date: 2026-02-18

This document defines a future-proof, multi-tier blob storage plan for agentd. The DB remains a metadata index; binary
blobs live in tiered stores with explicit retention, budgets, and deterministic references.

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

### Tier 2: Object store (S3/MinIO)

- Key: `blobs/sha256/<aa>/<bb>/<sha256>`.
- Presigned URLs for clients; agentd can proxy for auth-only environments.
- Optional local cache on read-through.

### Tier 3: Archive

- Optional cold storage for compliance.
- Read-through may be async with restore delay; surfacing a `restore_pending` state.

## Write path

1) Tool emits artifact → blob file stored in local tier.
2) Compute hash + size, register in `blob_manifest`.
3) Create `artifact_blobs` association.
4) Background mover enforces policies:
   - promote to object store
   - evict local based on LRU, size cap, or TTL

## Read path

- Resolve `blob_id` → tier + location.
- If local: stream with `Range` support.
- If object store: proxy or redirect to signed URL.
- Cache on read if policy allows.

## APIs (proposed)

Read:
- `GET /api/v1/blob/{blob_id}` (supports `Range`)

Upload:
- `POST /api/v1/blob/upload` (multipart or raw binary; returns `blob_id`)

Metadata:
- `GET /api/v1/blob/{blob_id}/meta`
- `POST /api/v1/blob/retain` (pin or set retention class)

These are additive; legacy `GET /api/v1/file` remains supported.

## Retention + GC policy

- Policies by tier, size, MIME, and age.
- Ref-counted GC: remove only when `ref_count == 0` and TTL expired.
- Optional "pin" labels for evidence bundles.

## Observability

- Counters: bytes per tier, objects count, cache hit/miss, promotion/eviction counts.
- Auditable events for GC and tier transitions.

## Phased delivery

1) **v0 (local-only)**: `blob_manifest` + local store + read endpoint + ref-count GC.
2) **v1 (object store)**: S3/MinIO backend + signed URLs + read-through cache.
3) **v2 (tiering)**: policy engine + background mover + size budgets.
4) **v3 (archive)**: cold storage restore workflow + operator controls.

## Compatibility

- Existing artifacts with `path` remain valid.
- New artifact JSON should include `blob_id` when available.
