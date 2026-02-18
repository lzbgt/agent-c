# Memory (Durable + Searchable)

`agentd` (and the `agent` CLI when connected to `agentd` state) uses a **durable memory directory** under the daemon `state_dir`:

- `state_dir/memory/MEMORY.md` — core “always true” memory (facts/preferences/tasks)
- `state_dir/memory/YYYY-MM-DD.md` — daily append-only log (bounded scan by days)
- `state_dir/memory/sessions/<session_id>.md` — optional session layer
- `state_dir/memory/STRUCTURED.md` — machine-maintained structured memory (via `memory_put(entries)`)
- `state_dir/memory/checkpoints/structured_<ts>.json` — time-stamped structured snapshots (best-effort, rolling)
  - each checkpoint has a deterministic `sha256` surface (exposed by APIs/tools)
- `state_dir/memory/recaps/recap_<ts>.json` — optional LLM-generated recap snapshots

## Tools

These are host tools exposed to the model when a daemon session context is available:

- `memory_write` — append a note (default: `layer="daily"`)
- `memory_observe` — append a structured observation (daily log; citations + tags)
- `memory_get` — read a memory file by path (paged by lines)
- `memory_search` — retrieve relevant snippets (prefer ranked search when available)
- `memory_timeline` — retrieve bounded context around a citation (`path:line`)
- `memory_put` — consolidate/overwrite memory files (legacy mode), or structured upsert (`entries`)

## Memory context injection (runs)

When `tools="host"`, agentd can inject a **durable memory context** as a system message before the run:

- File mode (default): reads Markdown memory files and injects a concise snapshot.
- Search mode: injects **ranked snippets** (claude-mem style) for the current prompt.

Run request knobs (file mode + search mode share the same memory root):

- `memory_context_mode`: `"files"` (default), `"search"`, or `"index"` (progressive disclosure; `"progressive"` alias supported).
- `memory_include_structured`, `memory_include_core`, `memory_include_daily`, `memory_include_session`
- `memory_daily_days` (clamped), `memory_total_cap` (clamped)
- `memory_search_query` (defaults to prompt when omitted)
- `memory_search_use_index`, `memory_search_case_sensitive`
- `memory_search_order` (`ranked` | `newest` | `oldest`; default `ranked`)
- `memory_search_max_results`, `memory_search_max_snippet_chars`, `memory_search_context_lines`
- `memory_search_fallback_to_files` (if search yields no hits)

Search mode injects a system message that starts with `DURABLE_MEMORY_SEARCH_CONTEXT` and includes:
- total durable memory bytes + approximate tokens,
- the latest recap timestamp/path (if available),
- a recap hint line (if recaps exist),
- the latest assistant timestamp + assistant hint (if available),
- citations in the form `[tier path:line]`.

This makes search results cost-aware and keeps the model aligned with the latest recap summary.

### Progressive disclosure index mode

`memory_context_mode="index"` injects a lightweight index instead of full content:

```
DURABLE_MEMORY_INDEX
- This is a lightweight index of durable memory files on disk.
- Token estimates are approximate (bytes/4). Use memory_search and memory_get for details.
- Total memory bytes: 25521 (~tokens=6380)
- Latest recap: 2026-02-17T12:10:44Z (recaps/recap_2026-02-17T12-10-44Z.json)
- Recap hint: Ongoing goals + most recent decisions...
- Latest assistant: 2026-02-18T19:23:11Z
- Assistant hint: Confirmed deployment changes and pending follow-ups...

[structured STRUCTURED.md] lines=42 bytes=9101 ~tokens=2276
[core MEMORY.md] lines=18 bytes=2100 ~tokens=525
[daily 2026-02-14.md] lines=120 bytes=14320 ~tokens=3580
```

This mirrors the claude-mem style of “show what exists + cost first,” keeping context lean while still
giving the agent enough signals to fetch the right details on demand.

### Salience mode (dynamic)

`memory_context_mode="salience"` injects a **ranked, compact** memory summary based on
recency + importance:

- Structured memory: latest checkpoint (`memory/checkpoints/structured_*.json`)
- Daily observations: `@obs` blocks (importance-tagged)

The daemon computes a deterministic salience score:

```
score = base_weight * exp(-age_days / half_life_days)
```

Use `/api/v1/memory/salience` to inspect the ranked items and tuning parameters.

Tuning knobs (daemon config / `/api/v1/config/update`):

- `memory.salience_daily_days`
- `memory.salience_max_items`
- `memory.salience_structured_max_items`
- `memory.salience_daily_max_items`
- `memory.salience_half_life_days`
- `memory.salience_importance_weight`

### `memory_search` tiers + citations

`memory_search` results now include:

- `tier`: `core` | `structured` | `session` | `daily` (or `other`)
- `citation`: `${path}:${line}` (stable, human-readable pointer)

Ordering:
- `ranked` (default): preserves FTS5 relevance ordering (or scan order for substring mode).
- `newest` / `oldest`: reorders **daily** results by date/line while leaving other tiers stable.

### 3-step retrieval (claude-mem style)

For large memory stores, use the progressive workflow:

1) `memory_search` to find relevant citations (`path:line`)
2) `memory_timeline` with the citation to get a bounded context window
3) `memory_get` if you need the full file or a larger slice

This keeps context lean while still allowing precise follow-up reads.

For progressive disclosure, set `tiered=true` to group results by tier and get rough token
estimates per tier (`tiers.<tier>.token_estimate`).

### Privacy filtering (`<private>` blocks)

To keep sensitive content out of durable storage, wrap it in `<private>` tags:

```text
User’s card number is <private>4242 4242 4242 4242</private>
```

Behavior:
- `memory_write` strips `<private>...</private>` blocks before writing.
- `memory_observe` strips `<private>...</private>` blocks before writing.
- `memory_put` strips private blocks from legacy text and from structured `entries[].value`.
- If all content is private, the write is skipped and the tool reports `skipped_private=true`.

## Ranked retrieval (Memory v2)

`memory_search` supports a ranked mode backed by an on-disk SQLite index under the memory root:

- index path: `state_dir/memory/.memory_index.sqlite3`
- default: `use_index=true`
- scope: results are restricted to the same file set the tool would otherwise scan (`daily_days`, core/session/structured)

When SQLite or FTS5 is unavailable at runtime, `memory_search` automatically falls back to a bounded substring scan.

## Structured consolidation + checkpoints

For durable “facts” that should survive long-running evolution, prefer:

- write daily/raw observations via `memory_observe` (preferred) or `memory_write(layer="daily")`
- periodically upsert stable facts into `STRUCTURED.md` via `memory_put(path="STRUCTURED.md", entries=[...])`

### Structured memory schema (v2)

`STRUCTURED.md` contains a machine block (JSON) delimited by:

- `<!-- AGENT_MEMORY_V1_BEGIN -->`
- `<!-- AGENT_MEMORY_V1_END -->`

The delimiter names are historical; the **payload schema** evolves. Current schema:

- `schema: "agent_memory_v2"`
- `items: { <key>: <record> }`

Each record keeps both:

- **current** value (`kind`, `value`, `status`, `updated_utc`, `observed_utc`, `sources[]`)
- **history** (`versions[]`) for superseded facts

Deterministic semantics:

- Same `kind/value/status` + same `source` → no-op (idempotent).
- Same `kind/value/status` + new `source` → evidence-only update:
  - appends to `sources[]` (deduped)
  - refreshes `observed_utc`
  - does **not** change `updated_utc` and does **not** add a new version.
- Different `kind/value/status` → supersede:
  - previous current is pushed into `versions[]` (newest-first) with `superseded_utc`
  - current becomes the new value and its `sources[]` starts from the incoming source.

Bounds (to keep files small):

- `sources[]` capped (oldest dropped)
- `versions[]` capped (oldest dropped)

### Deterministic promotion via `@mem` markers (rolling consolidation v2.1)

To enable **deterministic** rolling consolidation (no LLM required), you can write explicit markers into daily memory.
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

To run periodically, start `agentd` with:

- `--memory-consolidate-interval-ms <n>` (0 disables; default)
- `--memory-consolidate-daily-days <n>` (default: 14)
- `--memory-consolidate-keep-checkpoints <n>` (default: 100)

Structured updates produce a time-stamped checkpoint JSON snapshot under `memory/checkpoints/` by default:

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

To keep disk usage bounded, the daemon can enforce deterministic retention over daily logs and structured checkpoints:

- `POST /api/v1/memory/retention/enforce` (auth required)
  - supports `dry_run` + per-call overrides
- config knobs:
  - `memory_retention_interval_ms`
  - `memory_retention_daily_max_days`
  - `memory_retention_daily_max_bytes`
  - `memory_retention_checkpoint_max_days`
  - `memory_retention_checkpoint_max_count`
  - `memory_retention_structured_deprecate_days`
  - `memory_retention_structured_deprecate_max_entries`

See `docs/MEMORY_RETENTION.md` for details.

## Memory recaps (LLM summaries)

For operator-triggered summaries, agentd can generate recap snapshots from salience-ranked memory:

- `POST /api/v1/memory/recaps` — generate a recap (LLM) or `dry_run` to preview
- `GET /api/v1/memory/recaps` — list recap snapshots under `memory/recaps/`

Recaps use `summary_model` by default and can override with `model` or `summary_model` per request.
Each recap JSON includes the prompt inputs, the recap summary (JSON + text), and the ranked items used.

## API helpers (correlation)

In addition to the tool surface, `agentd` exposes correlation helpers:

- `GET /api/v1/memory/checkpoints` — list checkpoint snapshots + sha256 (bounded by time window)
- `GET /api/v1/memory/checkpoints?structured_path=STRUCTURED.md` — optional filter by structured file path
- `GET /api/v1/memory/correlate?trace_id=...` — find structured keys whose evidence sources mention `trace:<trace_id>`
  - optional filters: `structured_path=...` and `key_prefix=...`
- `GET /api/v1/memory/query?...&key_prefix=...` — bounded query over the **current view** of structured memory
  (reads the newest checkpoint in the requested time window)
- `GET /api/v1/memory/index` — lightweight index of memory files (paths + size/line/token estimates)
- `GET /api/v1/memory/salience` — ranked items by recency + importance (structured + @obs daily)

## WebUI Memory Explorer

The WebUI exposes a **Memory explorer** panel (collapsible) that directly calls the memory endpoints:

- Structured query (`/api/v1/memory/query`)
- Trace correlation (`/api/v1/memory/correlate`)
- Checkpoint listing (`/api/v1/memory/checkpoints`)

This panel is intended for operator/debug use and returns raw JSON so you can inspect structured memory state
and evidence hashes without leaving the UI.

## Deterministic workflow tasks

Durable workflows can query memory without invoking an LLM:

- `kind:"memory_correlate"` — bounded correlation by `trace_id` evidence against structured checkpoints
- `kind:"memory_query"` — bounded query of the structured **current view** by `key_prefix` (reads newest checkpoint)
