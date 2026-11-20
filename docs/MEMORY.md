# Memory (Durable + Searchable)

`agentd` (and the `agent` CLI when connected to `agentd` state) uses a **durable memory directory** under the daemon `state_dir`:

- `state_dir/memory/MEMORY.md` — core notes (keep declared current facts/preferences/tasks here)
- `state_dir/memory/YYYY-MM-DD.md` — daily append-only log (bounded scan by days)
- `state_dir/memory/sessions/<session_id>.md` — optional session layer
- `state_dir/memory/STRUCTURED.md` — machine-maintained structured memory (via `memory_put(entries)`)
- `state_dir/memory/checkpoints/structured_<ts>_<source>_<revision>.json` — time-stamped structured snapshots (best-effort, rolling)
  - each checkpoint has a deterministic `sha256` surface (exposed by APIs/tools)
- `state_dir/memory/recaps/recap_<ts>.json` — optional LLM-generated recap snapshots

## Tools

These are host tools exposed to the model when a daemon session context is available:

- `memory_write` — append a note (default: `layer="daily"`)
- `memory_observe` — append a structured observation (daily log; citations + tags)
- `memory_get` — read one file snapshot and its content revision (paged by lines); optionally request exact retained bytes
- `memory_search` — retrieve relevant snippets (prefer ranked search when available)
- `memory_timeline` — retrieve bounded context around a citation (`path:line`)
- `memory_put` — revision-checked full-text replacement or structured upsert (`entries`)

## Revision-checked publication

This contract applies to the host memory tools and daemon structured-memory
producers. It is source-qualified on Linux; production remains stopped. The
portable C kernel has no filesystem, lock, JSON or retrieval dependency.
[`memory_store.cpp`](../cli/src/memory_store.cpp) owns publication;
[`toolset_host_memory.cpp`](../cli/src/toolset_host_memory.cpp) owns the tool
contract and structured rendering. Consolidation and retention reuse that path.

A replacement must name the bytes from which it was derived. A content revision
is `sha256:` followed by the 64 lowercase hexadecimal digits of SHA-256 over the
**entire exact file**, including line endings. `absent` means no file; an existing
empty file has a SHA-256 revision. This identifies content, not a monotonic update
number: identical bytes have the same revision.

Use these steps when updating a source:

1. Call `memory_get` with `{"path":"STRUCTURED.md"}`. Save `data.revision` and
   inspect the required content. Use a small `max_lines` page when only the
   revision is needed; `memory_structured_query` also reports the revision of
   its captured document alongside filtered records. A missing source returns `ok=false`,
   `data.exists=false` and `data.revision="absent"`; absence is an expected
   starting state, not permission to replace an existing file.
2. Derive the change from that snapshot. Pass its revision as the required
   `expected_revision` in `memory_put`, together with `path` and either `text`
   or `entries`. For a known-new source, this example is illustrative tool
   input, not a shell command:

   ```json
   {
     "path": "STRUCTURED.md",
     "expected_revision": "absent",
     "entries": [{"key":"ui.layout","value":"Keep primary output visible"}]
   }
   ```

3. Check `ok`, `data.commit_state` and `data.revision`. Success includes
   `previous_revision`, the resulting `revision` and `source_id`. Identical
   structured content can return `no_changes=true` without a publication.
   A paged read still reports the hash of the complete captured file. For a
   subsequent page from the same bytes, pass that hash as `memory_get.revision`.
4. On `data.code="memory_revision_conflict"`, inspect `current_revision`, read
   the new snapshot and reconsider the change. Do not refresh the expected hash
   and resubmit an old derived value automatically. Independent work can continue.

The root-wide OS writer lock is `memory/.locks/writer.lock`. It serializes
`memory_put`, `memory_write` and `memory_observe` across threads and cooperating
processes. Lock acquisition checks cancellation, waits at most five seconds and
returns `memory_busy` when exhausted. Appends capture the head while holding the
lock. They reject structured machine-block files; use structured upserts there.
Each source read and replacement is bounded to 8 MiB (8,388,608 bytes).

For a changed document, the writer performs these operations in order:

1. Compare the expected revision with the captured head under the lock.
2. Retain the **previous authoritative bytes**, before replacing them, at
   `.revisions/<path-hash>/<content-hash>.blob`. Both hashes use SHA-256;
   `path-hash` hashes the supplied relative source path and `content-hash`
   hashes its raw bytes. The proposed replacement is not archived as history
   before it becomes authoritative. A corrupt existing archive object prevents
   replacement when that object is needed to retain the current head.
3. Create a unique staging file in the destination directory. Write all bytes
   and sync the file. Recheck the current head, then atomically replace its name
   and sync the parent directory. On POSIX, new private directories use mode
   `0700`; staged files and snapshots use `0600`, which the replaced head inherits.
   Existing directory permissions are not rewritten.
4. Prune old originals as bounded, best-effort maintenance. `keep_checkpoints`
   also selects the revision window, clamped to 2–1,000, default 100. Current and
   immediately previous content are protected. A maintenance failure cannot
   revoke a durable commit. Recognized abandoned staging files for the same
   destination are cleaned under the lock, with a 4,096-entry scan bound.

`source_id` is `memory:` plus SHA-256 of the canonical memory-root path, a newline
and the supplied relative path. It identifies this configured physical scope;
relocating the root does not preserve that identifier. Direct privileged edits,
in-place writes outside the store, directory replacement and external deletion
are outside the cooperating-writer guarantee. The final comparison detects an
observed external change; it is not an OS compare-and-swap against arbitrary
writers. Daily-file rotation/deletion and derived-cache invalidation remain
separate policy work.

| Observable result | Meaning and recovery |
| --- | --- |
| `ok=true`, `commit_state=committed` | The head and its required prior original were synced. Auxiliary checkpoint creation can still report `checkpoint_ok=false`. |
| `memory_revision_conflict`, `commit_state=not_written` | This attempt did not replace the source. Read and reconsider using the current revision. |
| `memory_busy` or `cancelled`, `not_written` | No publication occurred. Respect cancellation; use a later bounded attempt for busy state. |
| `memory_unavailable`, `not_written` | Staging, original retention or storage validation failed before replacement. Repair the reported storage cause before retrying. |
| `memory_publication_uncertain`, `published_unconfirmed` | Replacement happened but later durability/acknowledgment was interrupted. Read the head and compare revisions; do not replay blindly. The host executor also returns an error so the existing effect journal retains uncertainty. |

`memory_get` with `revision="sha256:..."` reads only that exact current or
retained snapshot. Paged display text omits line terminators and strips trailing
carriage returns; the reported revision always hashes the original complete bytes. A missing, pruned, linked or corrupt original fails explicitly;
it never substitutes the latest text. The lookup verifies the archived raw-byte
hash. Retrieval now adds source/revision/span references as described below.
Original availability is bounded by retention; an immutable identifier does not
promise indefinite storage or prove that a claim is true.

Consolidation captures the structured head before deriving updates and publishes
against that revision. It checks the tool's `ok` envelope, not only executor
success. Other scanned inputs are not yet one multi-file transaction. Retention
derives deprecations from the current `STRUCTURED.md`, rather than the newest
checkpoint, and carries the same captured revision into its write. It reports
failed writes instead of counting them as successful deprecations.

For native workflow updates, `expected_revision` is required in the persisted
`memory_put` task. A dependent write can use
`${task.M.json:/memory_put_response/data/revision}` from an earlier completed
write. Stale writes return `retryable=false` with `memory_error` details; the
scheduler keeps independent requirements runnable. See [workflow memory
updates](WORKFLOWS.md#deterministic-memory-update-task-kindmemory_put-v17).

### Reproduce the authority checks

On a Linux development host, from the repository root, with the host build
prerequisites in [OPERATIONS.md](OPERATIONS.md):

```sh
cmake -S . -B build-snapshot -DCMAKE_BUILD_TYPE=Debug -DAGENT_BUILD_HOST=ON -DAGENTD_ENABLE_HTTP=ON
cmake --build build-snapshot --target memory_store_tests host_toolset_tests agentd -j4
ctest --test-dir build-snapshot -R '^(memory_store_tests|host_toolset_tests|agentd_workflow_memory_put_smoke|agentd_memory_retention_smoke)$' --output-on-failure
```

Pass means all selected checks report success. The fixtures use isolated
synthetic state and no model provider. They exercise competing thread/process
writers, append preservation, expected-revision refusal, original retrieval,
archive failure, cancellation, size limits, interrupted publication, real
`SIGKILL` before/after replacement, workflow reference binding and stale
retention checkpoints. They do not simulate storage power loss. Windows uses
`LockFileEx`, `FlushFileBuffers` and write-through replacement in source, but
Windows runtime/durability qualification has not been performed. For another
filesystem or platform, rerun these checks and qualify its sync/rename behavior
before claiming equivalent durability.

## Current views and original evidence

[`memory_view.cpp`](../cli/src/memory_view.cpp) projects captured source bytes into
eligible passages. A passage is one structured value or a Markdown paragraph/observation
selected for retrieval. [`memory_index.cpp`](../cli/src/memory_index.cpp), structured
queries, salience and every context mode reuse this host-side contract. The
portable C core has no dependency on the projector, SQLite or JsonCpp.

Use `view="current"` for `memory_search`, `memory_structured_query`,
`memory_timeline`, native `memory_query` tasks and `/api/v1/memory/query` or
`/api/v1/memory/salience`. This is the default. Use `historical` to inspect older
or inactive material explicitly. `evidence` exposes identified original records
without promoting them to current knowledge. `memory_get` is an original-source
reader: its default raw output is labeled `view="evidence"`, suitable for editing
and inspection, rather than automatic current-fact injection.

Current eligibility is a declared-state rule, not semantic fact verification:

- Structured records need a non-null value and active status (absent status uses
  the schema default). Deprecated kinds, inactive/unknown statuses, malformed
  typed state, deletion/invalidation flags and future or invalid `valid_from`
  timestamps are excluded **before scoring**. Embedded `versions[]` are excluded.
- The structured machine block is authoritative. Generated Markdown that repeats
  its active/deprecated records is not indexed again. Only the explicit retained
  user-notes block adds independent Markdown passages.
- Raw Markdown excludes the declared `Deprecated`, `History`, `Historical`,
  `Superseded`, `Inactive`, `Obsolete`, `Deleted`, `Archived`, `已废弃`, `历史` and
  `已删除` sections, until the next heading at the same or a higher level.
  Explicit inactive `@mem` markers are excluded. Fenced/indented code is not
  interpreted as state headings. Unstructured notes without declared state remain
  observations; the system cannot infer every factual correction from prose.
- A raw `@mem` marker whose key already exists in root `STRUCTURED.md` is a
  promotion input, not an independent current authority. Current views suppress
  it; other views retain it as `historical=true`, `status=superseded_marker`.
  The structured head is a checked dependency even when its layer is not returned.

Historical/evidence search uses the selected documents' heads plus their retained
embedded versions. It does not scan every archived revision. For structured
queries, `include_versions=true` requires an explicit historical/evidence view
and returns separate labeled rows, each with its own reference. `status=any`
cannot bypass current eligibility. Missing observation time stays unknown;
reading a document no longer assigns the present time to old records.

A result's `evidence` object has schema `agent.memory.evidence.v1` and contains:

| Field | Meaning |
| --- | --- |
| `path`, `source_id` | Source within the configured physical memory root; the source ID binds that scope. |
| `revision` | SHA-256 of the complete original raw bytes, including line endings. |
| `span.kind=bytes` | Half-open raw byte offsets `begin` and `end`. |
| `span.kind=json_pointer` | RFC 6901 `pointer` to the exact value in the structured machine JSON, or in an original checkpoint JSON document. `/` and `~` in keys are escaped. |
| `id` | Server-generated SHA-256 identity over the compact JsonCpp serialization of the reference fields above, before adding `id`. Treat it as opaque; it is not a signature or authorization token. |

To retrieve the original, copy the **entire** returned `evidence` object into
`memory_get.evidence`. The reader verifies the source scope, raw-byte revision,
span and identity. It returns `data.value` and `data.text`. A correction cannot
redirect that reference to the current value. Missing, pruned, linked, corrupt,
altered or wrong-scope originals fail. Internal cache/lock metadata is not an
evidence source. `path:line` remains a human locator, not an immutable identity.
Use `memory_timeline` for current eligible neighbors, and `memory_get(evidence)`
for exact original evidence. Timeline context never expands into excluded raw
neighbors. A historical timeline can name an exact retained `revision`.

### Snapshot and cache boundaries

A retrieval captures complete documents, applies eligibility, then verifies the
selected revision set before returning output. A detected correction/deletion
clears search results; it does not return a mixture or retry a stale write.
The snapshot carries the UTC eligibility cutoff; automatic context always uses
current view. This establishes a checked read frontier, not a lock held until a
consumer acts and not protection against privileged in-place writes outside the
store. Consumers of long-lived conclusions still need revision validation.

Limits fail explicitly: 128 selected files, 8 MiB per document, 64 MiB captured
source bytes, 20,000 lines per document and 20,000 projected passages. The
structured dependency counts toward captured bytes. Search limits are 1–200
results, 80–4,000 snippet bytes and 1–4,096 query bytes. Snippet windows stay inside
eligible passages, respect `context_lines`, and retain references to the original
passage/value. They need not contain the entire original. UTF-8 clipping avoids
partial code points. Budget failures are errors, not silent source truncation.

The FTS5 cache is `.memory_retrieval_v1.sqlite3`, separate from the older unused
lexical cache. It stores **only the selected eligible projection** for one view
and source set. A SQLite transaction replaces a different scope/view before
BM25 is evaluated, so other sessions' rows do not enter scoring statistics.
Its signature binds policy version, mode, sorted paths/revisions, passage IDs
and projected text. Fields are streamed into SHA-256 with an eight-byte
big-endian byte-length prefix. Content changes invalidate it even when file
mtime is unchanged. POSIX cache mode is `0600`.

The SQLite busy wait is bounded to 500 ms. Missing FTS5, unavailable storage or
query/index errors use the **same captured eligible passages** for substring
fallback; `index_error` remains visible. No fallback scans raw history. A source
change still fails closed. Root-wide physical deletion/forget policy and persistent
cache erasure are separate from query eligibility; old cache files are not read
or migrated by this implementation.

### Reproduce retrieval checks

Use the host prerequisites and assertion-enabled build described above. From the
repository root on Linux:

```sh
cmake -S . -B build-snapshot -DCMAKE_BUILD_TYPE=Debug -DAGENT_BUILD_HOST=ON -DAGENTD_ENABLE_HTTP=ON
cmake --build build-snapshot --target agentd memory_view_tests host_toolset_tests -j4
ctest --test-dir build-snapshot -R '^(memory_view_tests|memory_retrieval_smoke|host_toolset_tests|agentd_memory_salience_smoke|agentd_memory_recaps_smoke|agentd_workflow_memory_query_smoke)$' --output-on-failure
```

Pass requires every selected test to succeed. The fixtures cover FTS and
substring paths, inactive/future/malformed records, embedded history, raw
sections/markers, source-set changes in a warm cache, Chinese/code identifiers,
mtime-preserving corrections, source deletion, same-content wrong scopes,
altered spans, exact originals and all four context modes. A real temporary
daemon checks current versus checkpoint reads and a correction during a synthetic
provider response. It makes exactly two loopback fixture calls; it does not use
credentials or a live model. A failed recap does not publish or replay its call.
These checks establish eligibility/binding behavior, not retrieval recall,
supported-answer accuracy, MCU behavior or a paid scorer baseline.

## Shared memory scopes

Runs can opt into a **shared memory scope** (for multi-agent collaboration) by setting:

- `memory_scope_id` — scope identifier (rooted at `state_dir/memory_scopes/<id>`).
- `memory_scope_mode` — `"read_write"` (default) or `"read_only"` (blocks memory writes).

When set, memory tools and memory-context injection read/write under the scope root instead
of the default `state_dir/memory`. This keeps team runs isolated from the global memory
unless explicitly configured to share a scope.

## Memory context injection (runs)

When host tools are enabled, automatic context uses the same current snapshot as
retrieval. Its root is the run's selected memory scope; another session's files,
global assistant-message hints and unversioned recap summaries are not injected.
Notes are evidence, not execution instructions or verified conclusions.

| `memory_context_mode` | Current behavior |
| --- | --- |
| `files` | Eligible passages with exact original references, rather than raw generated Markdown. |
| `search` | Scoped BM25/substring results from that snapshot; an optional files fallback uses the same projection. |
| `index` | Eligible passage counts, byte/token estimates and source revisions; no recap or assistant hint text. |
| `salience` | Recency/importance ranking from the authoritative structured head and daily observations, plus eligible core/session passages. |

Existing run controls select core, structured, daily/session layers, daily window,
result/snippet/context limits, ordering, salience weights and `memory_total_cap`.
The default output cap is 12,000 bytes, maximum 40,000. Whole evidence blocks fit
or are omitted; context never cuts a reference in half. A partial context says
its budget was reached. An unreadable, malformed, changed or over-budget source
produces `DURABLE_MEMORY_UNAVAILABLE` with the reason and a current-tool reread
instruction. An empty store simply contributes no memory context. Independent
work remains possible; these reads do not raise permission requests.

### Progressive disclosure index mode

Use the index to choose bounded follow-up reads. The safe sequence is:

1. `memory_search` or `memory_structured_query` in current view.
2. `memory_get` with the returned `evidence` for the exact original.
3. `memory_timeline` for eligible current neighbors, if needed.

### Salience mode (dynamic)

Salience reads `STRUCTURED.md`, not an optional checkpoint. Inactive material is
excluded before score calculation. For an eligible item:

```text
score = weight * exp(-age_days / max(0.01, half_life_days))
```

Structured weights are 1.0 for facts, 0.9 for preferences and 1.1 for tasks.
Observation weight is `1 + importance_weight * importance`, with importance
clamped to 0–5. Unknown timestamp uses one configured half-life as its scoring
age and leaves the timestamp empty. Scores indicate ranking relevance, not truth
or authorization. `/api/v1/memory/salience` returns original references and an
explicit `view`; the existing policy controls remain available.

### `memory_search` tiers + citations

Results retain `tier` (`core`, `structured`, `session`, `daily` or `other`), the
human `citation`, score/snippet and grouped token estimates when `tiered=true`.
They add `view`, `historical` and `evidence`; `as_of_utc` identifies the eligibility
cutoff when results exist. Ranked ordering preserves BM25/substring order;
newest/oldest ordering changes only daily results. The original reference, rather
than a live path and line, is the durable evidence address.

### Privacy filtering (`<private>` blocks)

To keep sensitive content out of durable storage, wrap it in `<private>` tags:

```text
User’s card number is <private>4242 4242 4242 4242</private>
```

Behavior:
- `memory_write` strips `<private>...</private>` blocks before writing.
- `memory_observe` strips `<private>...</private>` blocks before writing.
- `memory_put` strips private blocks from full-text input and from structured `entries[].value`.
- If all content is private, the write is skipped and the tool reports `skipped_private=true`.

## Ranked retrieval

The eligibility projection, cache transaction and fallback contract are defined
in [Snapshot and cache boundaries](#snapshot-and-cache-boundaries). The next
retrieval-quality experiment is gated in [the architecture](ARCHITECTURE_CURRENT.md#memory-authority-and-retrieval-evaluation).
No provider scorer, embedding model or replacement answerer is required for
current local lexical retrieval.

## Structured consolidation + checkpoints

For durable “facts” that should survive long-running evolution, prefer:

- write daily/raw observations via `memory_observe` (preferred) or `memory_write(layer="daily")`
- periodically upsert stable facts into `STRUCTURED.md` via `memory_put(path="STRUCTURED.md", expected_revision=<captured revision>, entries=[...])`

### Structured memory schema (v2)

`STRUCTURED.md` contains a machine block (JSON) delimited by:

- `<!-- AGENT_MEMORY_V1_BEGIN -->`
- `<!-- AGENT_MEMORY_V1_END -->`

The delimiter names are historical; the **payload schema** evolves. Current schema:

- `schema: "agent_memory_v2"`
- `items: { <key>: <record> }`

Each record keeps both:

- **current** value (`kind`, `value`, `status`, `updated_utc`, `observed_utc`, optional `valid_from`, optional `supersedes[]`, `sources[]`)
- **history** (`versions[]`) for superseded facts

Deterministic semantics:

- Same `kind/value/status` + same `source` → no-op (idempotent).
- Same `kind/value/status/valid_from` + new `source` or `sources[]` → evidence-only update:
  - appends to `sources[]` (deduped)
  - refreshes `observed_utc` (or uses explicit `entries[].observed_utc`)
  - does **not** change `updated_utc` and does **not** add a new version.
- Different `kind/value/status/valid_from` → supersede:
  - previous current is pushed into `versions[]` (newest-first) with `superseded_utc`
  - current becomes the new value and its `sources[]` starts from the incoming `source`/`sources[]`
  - current `supersedes[]` includes the previous version reference (`<key>@<updated_utc>`) plus any explicit `entries[].supersedes`.

Bounds (to keep files small):

- `sources[]` capped (oldest dropped)
- `versions[]` capped (oldest dropped)

### Deterministic promotion via `@mem` markers (rolling consolidation v2.1+)

To enable **deterministic** rolling consolidation (no LLM required), you can write explicit markers into bounded memory layers.
The daemon can then promote them into `STRUCTURED.md`:

Marker syntax (one per line, optional bullet prefix):

```text
@mem fact <key> = <value>
@mem pref <key> = <value>
@mem task <key> = <value>
@mem deprecated <key> = <value>
```

Example:

```text
- @mem fact ui.rendering = Scene rendering must survive refresh + restart
- @mem deprecated feature.a = Feature set A
```

On demand, call:

- `POST /api/v1/memory/consolidate` (auth required when daemon auth is enabled)

The on-demand endpoint scans core (`memory/MEMORY.md`), recent daily logs (`memory/YYYY-MM-DD.md`), bounded session notes
(`memory/sessions/**/*.md`), and human-readable structured-memory text by default. The structured-memory machine JSON block
between `AGENT_MEMORY_V1_BEGIN` / `AGENT_MEMORY_V1_END` is skipped so existing records are not recursively re-promoted; notes
outside that block can still carry explicit markers. Structured-file markers use the stable source label
`structured:STRUCTURED.md`, rather than line numbers, so repeated consolidation remains idempotent after `memory_put`
re-renders the file.

Useful request knobs:

- `include_core`, `include_daily`, `include_session`, `include_structured` (all default true)
- `daily_days` (default from daemon config)
- `session_days` (default follows `daily_days`)
- `max_session_files` (default 64; newest files by modification time)
- `max_file_bytes` (default 1 MiB per scanned file; clamped to at least 1024)
- `max_entries`, `dry_run`, and `keep_checkpoints`

To run periodically, start `agentd` with:

- `--memory-consolidate-interval-ms <n>` (0 disables; default)
- `--memory-consolidate-daily-days <n>` (default: 14)
- `--memory-consolidate-keep-checkpoints <n>` (default: 100)

Scheduled consolidation uses `--memory-consolidate-daily-days` as both the daily and session scan window, with the default
64-session-file cap.

After a successful structured publication, the optional auxiliary checkpoint is
published atomically without overwriting an existing object under
`memory/checkpoints/structured_<ts>_<source>_<revision>.json`. Its payload records
`source_id` and `revision`; different updates within the same second have different
paths. Checkpoint creation and pruning are best effort. They are not the
publication authority or a substitute for reading the current head:


- `checkpoint=true|false` (default: true)
- `keep_checkpoints=<N>` (default: 100)

When a checkpoint is written, `memory_put` also reports:
- `checkpoint_path` (relative to memory root)
- `checkpoint_ts_utc`
- `checkpoint_sha256` (sha256 of checkpoint JSON bytes)
- `checkpoint_bytes`

These fields exist so workflows can attach a stable “memory evidence hash” to their event logs without needing to
re-open files during execution.

## Memory retention (v1)

This is a deterministic, operator-controlled policy to keep durable memory growth bounded.

Goals:
- Bound disk usage for daily logs and structured checkpoints.
- Provide dry-run previews.
- Support both on-demand and background enforcement.
- Allow structured memory facts to be deprecated when they age out of policy.

Policy surfaces:
1) Daily logs (state_dir/memory/YYYY-MM-DD.md)
   - Age bound: keep at most daily_max_days files (oldest deleted first)
   - Size bound: keep total daily bytes under daily_max_bytes
2) Structured checkpoints (state_dir/memory/checkpoints/structured_*.json)
   - Age bound: delete checkpoints older than checkpoint_max_days
   - Count bound: keep at most checkpoint_max_count (oldest deleted first)
3) Structured deprecation (STRUCTURED.md)
   - Current authoritative entries older than structured_deprecate_days are revision-checked and upserted as status="deprecated"
   - Bound by structured_deprecate_max_entries per enforcement pass

API:
- POST /api/v1/memory/retention/enforce
  - supports dry_run plus per-call overrides
  - returns deleted files and counts

Example request:

{
  "dry_run": true,
  "daily_max_days": 30,
  "daily_max_bytes": 104857600,
  "checkpoint_max_days": 30,
  "checkpoint_max_count": 200,
  "structured_deprecate_days": 90,
  "structured_deprecate_max_entries": 50
}

Config knobs (all optional; 0 disables each bound):
- memory_retention_interval_ms
- memory_retention_daily_max_days
- memory_retention_daily_max_bytes
- memory_retention_checkpoint_max_days
- memory_retention_checkpoint_max_count
- memory_retention_structured_deprecate_days
- memory_retention_structured_deprecate_max_entries

Broker fan-out (multi-deployment):
- POST /v1/agents/{agent_id}/memory/retention/enforce

Background enforcement:
- when memory_retention_interval_ms > 0, the daemon applies policy periodically

## Memory recaps (LLM summaries)

For operator-triggered summaries, agentd can generate recap snapshots from salience-ranked memory:

- `POST /api/v1/memory/recaps` — generate a recap (LLM) or `dry_run` to preview
- `GET /api/v1/memory/recaps` — list recap snapshots under `memory/recaps/`

Recaps use `summary_model` by default and can override with `model` or `summary_model` per request.
Each recap JSON includes the prompt inputs, the recap summary (JSON + text), and the ranked items used.
Recaps include exact item `evidence` references, `source`/`evidence_sources` IDs
and a persisted `input_revisions` manifest. Inputs are validated again after the
provider response and before publication. A changed input rejects that result
without repeating the call. The manifest enables later stale-input checks;
provenance alone does not validate generated claims. Existing historical recap
inspection remains available, while automatic current context does not inject
recap text. No recap schedule or production model is enabled by this source change.

Optional request fields:
- `kind`: label the recap (e.g. `daily`, `weekly`, `manual`), used in the recap filename.
List filtering:
- `GET /api/v1/memory/recaps?kind=daily` filters the recap list by kind. Recaps without an explicit kind are treated as `manual`.

Scheduled recaps (disabled by default):
- `--memory-recap-daily-interval-ms <n>` / `AGENTD_MEMORY_RECAP_DAILY_INTERVAL_MS`
- `--memory-recap-weekly-interval-ms <n>` / `AGENTD_MEMORY_RECAP_WEEKLY_INTERVAL_MS`
- `--memory-recap-daily-days <n>` / `AGENTD_MEMORY_RECAP_DAILY_DAYS`
- `--memory-recap-weekly-days <n>` / `AGENTD_MEMORY_RECAP_WEEKLY_DAYS`

Scheduled recaps use the same salience tuning as manual recaps, but override `daily_days` per schedule.

## API helpers (correlation)

In addition to the tool surface, `agentd` exposes correlation helpers:

- `GET /api/v1/memory/checkpoints` — list checkpoint snapshots + sha256 (bounded by time window)
- `GET /api/v1/memory/checkpoints?structured_path=STRUCTURED.md` — optional filter by structured file path
- `GET /api/v1/memory/correlate?trace_id=...` — find structured keys whose evidence sources mention `trace:<trace_id>`
  - optional filters: `structured_path=...` and `key_prefix=...`
  - response includes `relationship_graph` (`agentd.memory.relationship_graph.v1`) linking memory keys to the queried trace via `correlates_trace`, plus workflow/task/job IDs and bounded source-excerpt nodes from evidence text
  - when the correlation index exists, the response also includes:
    - `daily_entries`: @obs daily lines linked to the trace_id
    - `recap_entries`: recap files that cite the correlated structured/daily evidence
    - `index`: index metadata (generated timestamps + counts)
- `POST /api/v1/memory/correlation/index` — build a cross-run correlation index (structured + daily + recap evidence)
  - the daemon also refreshes this index after recap generation or memory consolidation (best-effort)
  - index path: `memory/.memory_correlation.sqlite3` (SQLite builds) or `memory/.memory_correlation.json` (fallback)
- `GET /api/v1/memory/query?...&key_prefix=...` — bounded query over the **current view** of structured memory
  (reads the authoritative head by default; checkpoint windows require explicit historical/evidence view)
- `GET /api/v1/memory/index` — lightweight index of memory files (paths + size/line/token estimates)
- `GET /api/v1/memory/salience` — ranked items by recency + importance (structured + @obs daily)

## WebUI Memory Explorer

The WebUI exposes a **Memory explorer** panel (collapsible) that directly calls the memory endpoints:

- Structured query (`/api/v1/memory/query`)
- Trace correlation (`/api/v1/memory/correlate`)
- Correlation index build (`POST /api/v1/memory/correlation/index`)
- Checkpoint listing (`/api/v1/memory/checkpoints`)

This panel is intended for operator/debug use and returns raw JSON so you can inspect structured memory state
and evidence hashes without leaving the UI.

## Deterministic workflow tasks

Durable workflows can query memory without invoking an LLM:

- `kind:"memory_correlate"` — bounded correlation by `trace_id` evidence against structured checkpoints; results include the same `relationship_graph` shape as `/api/v1/memory/correlate`
- `kind:"memory_query"` — bounded query of the structured **current view** by `key_prefix` (authoritative head by default; explicit historical/evidence windows select checkpoints)

## Remaining authority work

The current view removes explicitly stale candidates and derived cache exposure;
it does not finish general correction/forget semantics. Automatic consolidation
still needs durable observation/provenance receipts to prevent an older `@mem`
marker from being promoted again after a later correction. Daily-file retirement
must retain required originals under a declared policy. Persistent cache erasure,
recap/conclusion invalidation at every consumer, bounded hard forgetting and
provenance-preserving handoffs remain open. Invalidating evidence must never
resubmit a completed external effect. Keep these requirements ahead of the paid
retrieval-quality pilot; do not interpret green eligibility tests as completion
of the broader memory-authority goal.
