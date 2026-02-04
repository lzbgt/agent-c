# Memory (Durable + Searchable)

`agentd` (and the `agent` CLI when connected to `agentd` state) uses a **durable memory directory** under the daemon `state_dir`:

- `state_dir/memory/MEMORY.md` — core “always true” memory (facts/preferences/tasks)
- `state_dir/memory/YYYY-MM-DD.md` — daily append-only log (bounded scan by days)
- `state_dir/memory/sessions/<session_id>.md` — optional session layer
- `state_dir/memory/STRUCTURED.md` — machine-maintained structured memory (via `memory_put(entries)`)
- `state_dir/memory/checkpoints/structured_<ts>.json` — time-stamped structured snapshots (best-effort, rolling)

## Tools

These are host tools exposed to the model when a daemon session context is available:

- `memory_write` — append a note (default: `layer="daily"`)
- `memory_get` — read a memory file by path (paged by lines)
- `memory_search` — retrieve relevant snippets (prefer ranked search when available)
- `memory_put` — consolidate/overwrite memory files (legacy mode), or structured upsert (`entries`)

## Ranked retrieval (Memory v2)

`memory_search` supports a ranked mode backed by an on-disk SQLite index under the memory root:

- index path: `state_dir/memory/.memory_index.sqlite3`
- default: `use_index=true`
- scope: results are restricted to the same file set the tool would otherwise scan (`daily_days`, core/session/structured)

When SQLite or FTS5 is unavailable at runtime, `memory_search` automatically falls back to a bounded substring scan.

## Structured consolidation + checkpoints

For durable “facts” that should survive long-running evolution, prefer:

- write daily/raw observations via `memory_write(layer="daily")`
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
