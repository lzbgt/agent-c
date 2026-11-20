# Memory (Durable + Searchable)

`agentd` (and the `agent` CLI when connected to `agentd` state) uses a **durable memory directory** under the daemon `state_dir`:

- `state_dir/memory/MEMORY.md` — core notes (keep declared current facts/preferences/tasks here)
- `state_dir/memory/YYYY-MM-DD.md` — daily append-only log (bounded scan by days)
- `state_dir/memory/sessions/<session_id>.md` — optional session layer
- `state_dir/memory/STRUCTURED.md` — machine-maintained structured memory (via `memory_put(entries)`)
- `state_dir/memory/checkpoints/structured_<ts>_<source>_<revision>.json` — time-stamped structured snapshots (best-effort, rolling)
  - each checkpoint has a deterministic `sha256` surface (exposed by APIs/tools)
- `state_dir/memory/recaps/recap_<opaque-id>.json` — optional LLM-generated recap snapshots

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
   and sync the file. Recheck the current head, take the cache lease and durably purge registered disposable caches, then atomically replace its name
   and sync the parent directory. On POSIX, new private directories use mode
   `0700`; staged files and snapshots use `0600`, which the replaced head inherits.
   Existing directory permissions are not rewritten.
4. Keep original revisions. Ordinary writes no longer prune history by a hidden
   count/mtime policy. `keep_checkpoints` controls active checkpoint files only.
   Explicit [archive expiry](#explicit-archive-expiry) is a separate bounded
   operation. Recognized abandoned staging files for the same destination are
   still cleaned under the lock, with a 4,096-entry scan bound.

`source_id` is `memory:` plus SHA-256 of the canonical memory-root path, a newline
and the supplied relative path. It identifies this configured physical scope;
relocating the root does not preserve that identifier. Direct privileged edits,
in-place writes outside the store, directory replacement and external deletion
are outside the cooperating-writer guarantee. The final comparison detects an
observed external change; it is not an OS compare-and-swap against arbitrary
writers. Revision-checked daily/checkpoint retirement is described below. Registered cache artifacts are purged before publication; general forgetting remains separate policy work.

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

Consolidation captures all selected originals and the structured head, then
revalidates them under the same root writer lock. Facts and durable promotion
receipts share one structured publication. This serializes cooperating memory
writers; it does not turn arbitrary filesystem writes into a multi-file transaction.
It checks the tool's `ok` envelope and promotion completion separately. See
[durable marker promotion](#durable-marker-promotion). Retention
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

### Bounded evidence retrieval

This **host-only foreground research path** implements lexical candidates,
typed relevance scoring and at most one source-grounded expansion. It is not
enabled in normal `memory_search`, daemon context injection or the WebUI. The
existing 4B answerer is unchanged. The source contracts are
[`memory_retrieval.h`](../cli/src/memory_retrieval.h),
[`memory_retrieval.cpp`](../cli/src/memory_retrieval.cpp),
[`typesafe_scorer.cpp`](../cli/src/typesafe_scorer.cpp) and the executable
[`memory_retrieve.cpp`](../tools/memory_retrieve.cpp). No new model weights,
vector database, scheduler or portable-C dependency is introduced.

The same captured scope, source revisions, eligibility cutoff and passage
chunking serve every stage. Current mode excludes ineligible records before
scoring and term selection. Historical/evidence context marks historical rows
explicitly. Cancellation or a changed source clears returned context. A
successful relevance score is neither truth, permission nor an external-effect
receipt.

The three research arms have one shared lexical candidate policy:

1. **Lexical:** extract at most 32 distinct question terms, remove common English
   question words and issue a quoted OR query through the existing FTS5/BM25
   index. The original question remains intact for scoring. The same eligible
   substring fallback handles an unavailable index. Oversized/omitted question
   terms are counted and make the result explicitly incomplete. Existing direct
   lookup callers retain their FTS syntax; this mode is explicit.
2. **Rerank:** score at most 20 candidates against the original question. The
   pluggable host callback must return exactly one finite probability in `[0,1]`
   per supplied evidence ID and identify its provider, model and prompt revision.
   Missing, duplicated or foreign IDs, invalid scores and exceptions produce a
   visible incomplete lexical fallback without a scoring retry.
3. **Expand:** retain at most five first-stage candidates. Seeds scoring at
   least 0.5 contribute literal terms from their scoring text. Rank terms by
   captured passage frequency with deterministic text ties; select eight by
   default, at most 20. Search each term, interleave lists, exclude every initial
   candidate ID and admit at most 20 unseen passages. Rescore these with the
   retained candidates and accumulated evidence, then select final context.
   There is no second expansion or generated search term.

Every admitted expansion records `seed_id`, the literal `term`, a zero-based
half-open UTF-8 byte `seed_text_span` into that seed's recorded scoring text, and
`result_evidence`. Stage inputs, exact original references, validated scores and
usage remain in the report. Fetch the original with `memory_evidence_read`;
the scoring-text span is not a live `path:line` reference or a substitute for
the original source/revision/span identity. Snippets disclose `text_clipped`
and original passage byte length. No Unicode word segmenter is installed:
mixed Chinese/code lookup is tested; arbitrary Chinese question segmentation
and retrieval quality are not qualified.

Final context contains at most eight complete records, within **16,000 bytes of
compact JSON** by default. References are never cut to make a record fit.
`context_omitted`, `selection_omitted`, `question_terms_omitted` and
`terms_not_selected` distinguish output/admission budgets from ranking choices.
`context_budget`, `query_budget` and `lexical_fallback` are incomplete results.
An unavailable/failed second scorer preserves completed first-stage scores in
the report and returns the original lexical context. The research batch stops
at its first incomplete arm and retains preceding results.

#### Run a local lexical example

Use the host prerequisites from [Operations](OPERATIONS.md), an assertion-enabled
build and a **new example directory**. The following creates public synthetic
data only; it does not read a live Agent-C state directory:

```sh
cmake -S . -B build-snapshot -DCMAKE_BUILD_TYPE=Debug -DAGENT_BUILD_HOST=ON -DAGENTD_ENABLE_HTTP=ON
cmake --build build-snapshot --target agent-memory-retrieve memory_retrieval_tests typesafe_scorer_tests -j4
test ! -e build/retrieval-demo
mkdir -p build/retrieval-demo/memory
cat > build/retrieval-demo/memory/MEMORY.md <<'EOF'
Cedarport supplier is JuniperWorks.

JuniperWorks is based in Riverton.
EOF
cat > build/retrieval-demo/input.json <<'EOF'
{"schema":"agent.memory.screen.input.v1","files":["MEMORY.md"],"cases":[{"id":"example","question":"Where is Cedarport supplier located?","view":"current"}]}
EOF
build-snapshot/agent-memory-retrieve --memory-root build/retrieval-demo/memory \
  --manifest build/retrieval-demo/input.json --lexical-only \
  --output build/retrieval-demo/lexical.json
```

Pass requires exit 0, `status="complete"`, a lexical arm with revision-bound
context, and null provider usage. No provider credential is needed. This is a
tuning/operation example, **not** the held-out screening set or an answerer test.
The manifest accepts 1–20 distinct case IDs, original questions and explicit
current/historical/evidence views. A case's optional `files` can only narrow the
declared corpus. Gold labels are not sent to the scorer. Snapshot signatures
and source revisions bind all three arms; a detected corpus change stops work.

#### Bounded TypeSafe baseline

Only the explicit `--typesafe-env` research path reads the exact
`TYPESAFE_API_KEY` entry. The loader treats quotes/comments as dotenv data,
rejects duplicate/interpolated/invalid entries and imports no other values.
Do not source or print the file, pass the key in argv, copy it into the corpus
or put it in a prompt. The endpoint is fixed to the official
`https://api.typesafe.ai/v1/systemone`; credentials are never redirected to the
OpenRouter endpoint in the SAG2 example. TLS verification stays enabled,
redirects are disabled and environment proxies are disabled for this client.

The current adapter requests **`jev-1.14.0`** and requires the returned model to
match. It uses named `noul` questions and records the actual returned model,
prompt revision, validated token usage and bounded attempt receipts. A changed
model pin stops further scoring. The
[architecture evaluation gate](ARCHITECTURE_CURRENT.md#memory-authority-and-retrieval-evaluation)
links the inspected official API, model and price documents; recheck these facts
before any paid batch.

One serial client owns the entire cloud-arm budget, not one client per query:

| Bound | Foreground screen | Separate canary |
| --- | --- | --- |
| HTTP attempts, including retry | 75 | 2: account discovery and one scoring request; no retry |
| Candidate judgments, including retry | 1,300 | 2 |
| Accounted input tokens | 4,915,200 | 65,536 |
| Estimated ceiling at $0.042 / million input tokens | $0.2064384 | $0.002752512 |
| HTTP request / total client timeout | 15 s / 600 s | 15 s / 30 s |
| Request / response body limit | 32,768 / 65,536 bytes | same |

Before each scoring attempt the client reserves **65,536 input tokens** against
the total cap, conservatively interpreting the provider's default 64k request
context. The long-context header is never sent. Successful validated usage
replaces the reserve; a timeout, transport/schema failure or unknown outcome
keeps its reserve. Output tokens are recorded but currently free at the listed
price. These are client admission/accounting bounds and cost estimates, not a
provider billing receipt or a tokenizer-based prediction. Out-of-range usage
disables further scoring. HTTP 429/529 permit at most one retry after a bounded
250–2,000 ms wait. HTTP 499, authentication/payment errors, response overflow
and unknown network outcomes are never automatically retried. Cancellation and
total deadlines cover backoff and HTTP progress; arbitrary host callbacks and
kernel I/O remain cooperative rather than hard real-time.

The separately authorized small canary command is:

```sh
build-snapshot/agent-memory-retrieve --canary --typesafe-env "$HOME/.env" \
  --output build/retrieval-canary.json
```

Use a previously absent output path. The program first saves a revision-checked,
synced `prepared` receipt with its budgets and public two-passage fixture. Only
then does it discover account models and send one scoring request. Exit 0 proves
authentication and typed scoring plumbing; it does not prove answer quality.
Inspect `status`, `returned_model`, `scores` and `usage` on completion. An existing
output is refused even if its last state is `prepared` or `in_flight`: inspect
that original and reconcile unknown usage instead of creating another path to
replay a paid call. Earlier completed arms and original receipt revisions are
retained through the existing atomic store. The command has no automatic paid
resume or full-benchmark mode.

After freezing the independent 20-question corpus/labels, the same executable
accepts `--memory-root`, `--manifest`, `--typesafe-env` and a new `--output` to run
lexical/rerank/one-expansion arms. It records retrieval results only. Keep the
existing 4B answerer and fixed final context allowance for separate supported
answer/citation evaluation; no answerer replacement or quality promotion is
implied. Production daemon/gateway/PostgreSQL remain stopped.

#### Verify failure boundaries

From the repository root, after the build above:

```sh
ctest --test-dir build-snapshot -R '^(memory_retrieval_tests|typesafe_scorer_tests|memory_retrieval_research_smoke)$' --output-on-failure
sha256sum build-snapshot/agent-memory-retrieve
```

These checks exercise a real unseen-record bridge, exact evidence reads,
current/historical and scope restrictions, malformed IDs/scores, context/input
caps, cancellation/deadline, source corrections and lexical fallback. The
TypeSafe fixture uses synthetic credentials and loopback HTTP for response
overflow, timeout and cancellation; error bodies never reach reports. The CLI
fixture verifies retained prepared/completed receipts and refusal to replay an
existing output. They do not call a paid model, measure general retrieval/answer
quality or qualify an MCU. For another scorer, implement `MemoryScorer`, obey its
cancellation contract and repeat these failure cases before any quality study.

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

The FTS5 cache is `.memory_retrieval_v2.sqlite3`. It contains only one selected
eligible scope/view. A transaction replaces a changed projection before BM25
scoring. The signature binds policy version, mode, paths/revisions, passage IDs
and text using SHA-256 over length-prefixed fields. POSIX mode is `0600`.
SQLite 3.42 or newer with FTS5 is required for the indexed path. The connection
uses `journal_mode=DELETE`, `temp_store=MEMORY`, verifies `secure_delete=1` and
sets FTS5 `secure-delete=1`. FTS token deletion and core page scrubbing are separate
controls; see [FTS5 secure deletion](https://www.sqlite.org/fts5.html#the_secure_delete_configuration_option)
and [SQLite secure_delete](https://www.sqlite.org/pragma.html#pragma_secure_delete).
Older/unavailable SQLite falls back with a visible `index_error`.

A cross-process cache lease at `.locks/cache.lock` bounds acquisition to 500 ms.
Writers take the root writer lock, then the cache lease; readers take only the
cache lease. Readers never wait for the writer lock. Source replacement,
retirement and new immutable auxiliary publication remove these registered
application artifacts **before** publishing the change and hold the lease through
that boundary:

- `.memory_index.sqlite3`, `.memory_retrieval_v1.sqlite3`,
  `.memory_correlation.sqlite3` and `.memory_retrieval_v2.sqlite3`;
- their `-journal`, `-wal` and `-shm` sidecars;
- `.memory_correlation.json`.

The root directory is synced after removal. Disposable cache bytes are not
archived. Original source bytes remain under the separate revision policy.
Current readers also discard obsolete cache names. The next FTS query rebuilds
from eligible captured sources. This coarse invalidation favors a single clear
publication boundary over incremental cache maintenance; frequent writes can
increase query rebuild cost. It does not claim storage-device, snapshot or backup
erasure. Mixed old/new executables and privileged out-of-protocol writes are not
qualified; use a fresh deployment when that is authorized.

Cache contention or an invalid cache artifact prevents the source change with a
visible error. A partial purge is harmless because caches are rebuildable. On
`memory_busy`, reread the source before retrying. An unexpected directory/link at
a registered cache path requires local inspection; the store never follows it or
removes unrelated files. Index/storage/query failures use the same eligible
captured passages for substring fallback and retain `index_error`. A source change
still discards the result. An identical-byte no-op needs no publication purge.

Structured decoding preserves exact stored JSON. It rejects duplicate keys,
bounds nesting to 128 and scans quoted strings before recognizing the outer end
marker. Current reads/writes require `agent_memory_v2` and an `items` object;
unsupported schemas remain available through explicit historical/evidence reads. Records with the obsolete `valid_from_utc` alias are ineligible in current view; a read never silently removes a time constraint.
Missing fields are not synthesized during reading, so a JSON Pointer cannot cite
invented metadata. Only explicit notes after the complete machine block enter
independent Markdown retrieval. A write may deliberately add current-schema
fields, producing a new revision.

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
provider response. It makes exactly five loopback fixture calls, including an oversized response, HTTP 503 and an output-token limit; it does not use private credentials or a live model. Incomplete provider outcomes remain historical receipts without replay.
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

### Durable marker promotion

This host-only operation promotes explicit observations into `STRUCTURED.md`
without a model call. It uses the existing consolidation engine and atomic writer,
not another scheduler or database. The source is
[`memory_promotion.cpp`](../cli/src/memory_promotion.cpp); daemon selection is in
[`memory_consolidator.cpp`](../daemon/src/memory_consolidator.cpp).

An old observation must not become a new correction merely because consolidation
reads it again. The machine block therefore contains `promotion_ledger`, schema
`agent.memory.promotion.v2`, with up to **4,096 recent full decision receipts**
and **32,768 compacted consumed IDs**. When another selected key needs room,
consolidation replaces the prior full receipt table with exact consumed-ID
references to that table's retained original. It never drops consumed IDs.
Accepted facts and their receipts are published in the same atomic document.
A process interrupted before replacement leaves neither; after replacement both
are present. Retrying the scan recognizes the consumed observation. This is
process-crash qualification, not proof against storage power loss.

This is the current source format for fresh state. There is no automatic v1
conversion. An existing incompatible ledger is refused by current publication
and promotion capture. Preserve its source and archives for historical reads;
do not delete its ledger and rescan old markers as new work. No live memory was
converted for this milestone.

Use this sequence to create or replace a fact:

1. For a new key, append one marker to core, daily or session Markdown using
   `memory_write`. These are illustrative tool payload lines, not shell commands:

   ```text
   @mem fact ui.rendering = Keep primary output visible
   @mem pref response.language = Chinese
   @mem task release.check = Run the canonical verification lane
   @mem deprecated feature.a = Retired feature
   ```

   Plain markers can create a missing key. They cannot replace an existing key,
   even with an identical value. Equivalent new observations from several sources
   can create one fact with several source references. Differing observations for
   one key are rejected as `competing_observations`; file order and modification
   time do not choose a winner.
2. To replace an existing key, read it through `memory_structured_query` and copy
   the row's `record_revision`. This is SHA-256 of compact JsonCpp serialization
   of the complete parsed stored record, including its history and provenance. Reads do not migrate or manufacture record fields.
   It differs from the raw file's `data.revision`; another key's change does not
   invalidate it. For an inactive head use an explicit historical/evidence query.
   Create a new JSON marker on one line:

   ```text
   @mem {"key":"ui.rendering","kind":"fact","value":"Use the revised layout","expected_record_revision":"sha256:<64 lowercase hexadecimal digits copied from the row>"}
   ```

   The placeholder is illustrative and must be replaced with an actual returned
   hash. `expected_record_revision="absent"` explicitly binds a new key. Unknown
   JSON fields, wrong types and malformed revisions are reported. No permission
   prompt is added: the agent can inspect and issue a new justified intent within
   its authorized task. A revision conflict requires reconsideration, never an
   automatic refresh of the hash for the old value.
3. Call authenticated `POST /api/v1/memory/consolidate`, or use the native
   `memory_consolidate` workflow task. Both use the same operation. Inspect
   `memory_put_response` for actual publication and `promotion.complete` for
   semantic completion. A rejected observation is also consumed durably, so it
   cannot become effective later when the head changes back. Original rejection
   receipts remain unchanged; an observation reports `unresolved=true` while the
   current record still matches the head against which it was rejected.
4. On incomplete promotion, inspect `promotion.decisions`, `errors`, `unresolved`
   and `deferred`. HTTP returns `400` with the full `data` report; native tasks
   return `retryable=false` with `report`. **Independent accepted keys may already
   be committed.** Read their original receipt before recovery. Resolve the cause
   or issue a new bound intent; do not roll back or blindly replay a partial result.

### Compacted receipts and recovery

Compaction reuses the root writer transaction and exact original reader. The
current ledger contains three fields in addition to its schema:

| Field | Meaning and bound |
| --- | --- |
| `receipts` | At most 4,096 full immutable decisions, indexed by marker ID. |
| `consumed` | At most 32,768 exact marker IDs, each mapped to an integer `receipt_sources` index. These IDs remain consumed even if their originals cannot be read. |
| `receipt_sources` | At most 128 anchors containing physical `source_id`, raw structured-file `revision` and unique original marker `evidence_revisions`. Every anchor must be referenced. |

One pass can compact the full receipts from its captured structured head once.
Receipts created during that pass remain full; they cannot refer to an
unpublished intermediate state. The old head is retained before publication.
The atomic replacement publishes compacted IDs, the new decisions and changed
facts together. A crash before replacement leaves the old authority; a crash
after replacement leaves the new authority and its retained original.

For a previously consumed marker, the reader verifies the anchored original's
raw hash and physical scope, then reads its literal receipt at JSON Pointer
`/promotion_ledger/receipts/<marker-ID>`. Each original table is decoded once per
pass. Reports identify `receipt_storage="archived"` and supply
`original_receipt_evidence`, usable with `memory_evidence_read`, alongside the
receipt's original marker-byte evidence. The reader does not reconstruct a
decision from today's fact or follow a recursive summary chain.

The anchor's marker revision pins protect original leaf evidence even after a
direct correction removes the old derivation. The existing archive-expiry scan
recognizes both these literal revisions and the anchored structured revision.
Compaction reduces the current authority's full receipt bodies; it does **not**
reduce total archive disk usage. Old full snapshots and pinned original markers
remain subject to the explicit history policy.

If an original is missing, corrupt, from another scope or exceeds the lookup
budget, promotion fails before publication and the ID stays consumed. Restore
the exact required original from a verified copy and retry a fresh capture.
Never replace it with current bytes or remove the ID to permit a replay. Other
direct fact updates and independent workflow requirements remain available.

Detailed output is capped at 4,096 decisions, ordered by new decisions, unresolved
old decisions, then ordinary duplicate acknowledgements. `observations_checked`,
`decisions_omitted` and `details_incomplete` expose omitted detail. Aggregate
`complete`, `unresolved` and `deferred` cover the whole scan; `complete=true` does
not promise that every duplicate is printed. `compacted_receipts`,
`consumed_receipt_count`, `receipt_source_count`, `archive_reads` and
`archive_bytes` report this pass's capacity and original reads. These are input
provenance and execution results, not verification that a fact is true.

### Marker identity, eligibility and operating bounds

A marker identity is `marker:` plus SHA-256 of compact JSON containing schema
`agent.memory.marker.v1`, physical `source_id`, normalized key/kind/status/value
and optional expected record revision. Its identity excludes line number and
whole-file hash: moving a line or appending unrelated text does not refresh an
old intent. Each receipt independently retains an original evidence reference
with source, raw-byte revision and exact line byte span `[begin,end)`. Required
captured originals are synced into the existing revision archive before fact
publication. Original availability still follows the store's retention policy;
references do not promise indefinite storage.

A promoted head carries `derivation` (`agent.memory.marker_derivation.v1`).
Current retrieval checks that every supporting marker still exists, remains
eligible and agrees with the promoted key/value/kind/status. The dependency
heads participate in the revision fence and cache signature. Unrelated text
edits refresh supporting byte references without creating a new observation.
Changed, removed or historical markers exclude the derived head before FTS,
fallback, salience, structured/API/native queries or context injection. Broad
views retain the head with `historical=true` and
`eligibility_reason="promotion_source_changed_or_unavailable"`, plus the original
provenance. A direct core correction preserves the previous derivation in history
and removes it from the new head. These reads never dispatch or replay effects.

Selection and limits are explicit:

- `include_core`, `include_daily`, `include_session`, `include_structured` default
  true. Structured machine files contribute only their explicit
  `AGENT_MEMORY_V1_NOTES_BEGIN` / `AGENT_MEMORY_V1_NOTES_END` block. Generated
  Markdown, JSON, code fences, indented code and historical sections are excluded.
  Converting plain Markdown to a structured document preserves it in that notes block.
- `daily_days` defaults from daemon configuration and is clamped to 0–90.
  `session_days` selects a time window; `max_session_files` selects at most 64
  recent files by modification time. Session traversal fails past 4,096 entries.
  These timestamps select files, not authoritative values. Hidden session paths
  are skipped; linked sources fail validation. Both `.md` and `.markdown` are supported.
- At most 128 captured sources including the structured dependency, 64 MiB total,
  8 MiB per document, 20,000 lines per source, 4,096 distinct new intents,
  40,960 total marker observations and 64 parse errors. Already-consumed IDs do
  not use new-intent admission. Dependency validation allows at most 20,000
  references and 40,960 scanned markers. Original receipt lookup shares a 64-MiB
  byte allowance with the captured current documents. Exceeding a hard capture
  or lookup budget fails explicitly; input is never silently truncated.
- `max_file_bytes` defaults to 1 MiB, is at least 1,024 bytes and cannot exceed
  the store's 8 MiB read limit. The structured authority always uses that 8-MiB
  store limit, so its growing ledger cannot be rejected by the default 1-MiB
  observation-file limit before compaction. `max_entries` limits keys per invocation to
  1–1,000 (default 256); unprocessed observations report `deferred` and remain
  eligible for a later bounded scan. If the compacted-ID or anchor capacity
  prevents further compaction, new observations that do not fit remain deferred.
  Byte, source and line limits can be reached before the ID limits. Do not delete
  or reset consumed IDs to make room. This is bounded rolling capacity, not
  unlimited history or a general forget policy. Direct authorized upserts and
  independent workflow requirements remain available.
- `dry_run=true` only reads sources and reports `promotion_preview` with proposed
  decisions and `would_apply_keys`, `would_record_receipts` and
  `would_compact_receipts`. New decision rows have `proposed=true`; preview record
  revisions are not promised post-write hashes. It does not create locks,
  archives, facts or receipts. Actual
  execution rechecks the sources; preview is not a publication receipt.
- Full-text `memory_put` replacement of an existing machine document is rejected.
  Use `entries` so the ledger survives ordinary corrections. A malformed ledger
  stops publication; restore a verified original and reconcile later edits instead
  of resetting the ledger. Privileged out-of-protocol replacement remains outside
  the writer contract.

From the repository root on the Linux host, with the earlier host prerequisites:

```sh
cmake --build build-snapshot --target memory_promotion_tests memory_view_tests host_toolset_tests agentd -j4
ctest --test-dir build-snapshot -R '^(memory_promotion_tests|memory_view_tests|host_toolset_tests|agentd_memory_consolidate_smoke|agentd_workflow_memory_consolidate_smoke)$' --output-on-failure
```

Pass requires all selected checks to succeed. Synthetic fixtures cover later
corrections beyond the 20-version window, line movement, anchored updates,
conflicting/malformed observations, independent partial commits, rejected-intent
recovery, original retrieval after edits/deletion, every current consumer, stale
snapshots, read-only policy, cancellation, archive failure, byte/key/ledger caps,
notes conversion and real `SIGKILL` immediately before/after replacement. The
real daemon-consolidator test admits 4,096 genuine markers over five bounded
batches with the default observation-file limit, then appends two markers and
verifies compaction, old/new eligibility, omitted-detail counts and a no-change
retry. Separate synthetic full-table fixtures exercise exact original receipt
and leaf reads, archive-expiry pins, corrupt-original refusal, rejected-intent
non-replay after head restoration, the 32,768-ID cap and both compaction crash
boundaries. Filler receipts in those capacity fixtures are structurally valid
copies, not claims of independent factual evidence. The
HTTP fixture checks the incomplete result and committed independent fact. No
production service, live provider, paid scorer, filesystem power cut or MCU is
exercised. On another filesystem, repeat the same failure cases and qualify its
sync/rename semantics before extending the durability claim.

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

## Source retirement and retention

Retention removes selected **active file heads** after preserving their exact
original bytes. It applies to host daily logs and structured checkpoints. The
existing background engine and authenticated `POST /api/v1/memory/retention/enforce`
share this operation; no new scheduler or model call is added. Source:
[`memory_store.cpp`](../cli/src/memory_store.cpp),
[`memory_retention.cpp`](../daemon/src/memory_retention.cpp) and
[the API handler](../daemon/src/memory_endpoints.cpp).

The original implementation directly removed files and counted failed removals
as successful deletions. A synthetic regression demonstrated an unpromoted
original becoming unavailable. Retirement now follows the same authority as
publication, so a concurrent cooperating writer cannot lose a newer revision to
an older cleanup decision. This finding is from local source/fixtures; it does
not establish a production race.

### Policy and data flow

Daily selection uses valid 1970–9999 `YYYY-MM-DD.md` calendar dates. `daily_max_days=N`
keeps names at or after the local date obtained by subtracting `24*(N-1)` hours
from the current time. `daily_max_bytes` selects additional oldest logs until
captured active bytes fit the bound. This is a filename-based operational rule,
not proof of an observation's event date. Checkpoint selection reads strict
`ts_utc` values from captured JSON. `checkpoint_max_days` removes older entries;
`checkpoint_max_count` keeps the newest eligible timestamps, with ascending path
as the deterministic tie-breaker. Invalid timestamps are preserved and reported.
Selected removals run oldest first.

Each selected source carries a complete raw-byte revision from capture. Under
`memory/.locks/writer.lock`, the transaction compares that revision, verifies and
syncs its original at `.revisions/<path-hash>/<content-hash>.blob`, rechecks the
head, atomically unlinks its active name and syncs the parent directory. Hashes
and byte ranges follow [revision-checked publication](#revision-checked-publication).
Source paths must be normalized relative paths; redundant separators, trailing
slashes, traversal and linked components are refused. Empty files have a content
hash and can also be retired; absence remains a separate state.

After retirement, ordinary reads see `revision="absent"`. Exact original reads
through `memory_get(evidence=...)` or `memory_get(path=...,revision=...)` still use
the retained bytes. Current retrieval excludes the missing raw source, and
marker-derived facts whose required support disappeared become historical before
scoring/injection. Archiving alone does not keep that derived head eligible.
Historical references continue to identify the original. Retirement does not
execute, reconcile or replay an external workflow effect.

The active byte/count bounds exclude revision archives. **They are not a total
disk quota or hard-forget guarantee.** Retirement leaves retained originals in
place. Explicit archive expiry below determines when unreferenced originals can be removed. Inspect archive growth
before enabling a schedule on a storage-constrained host. No schedule or
production workload is enabled by this source change.

### Apply, inspect and recover

Start from the host prerequisites in [OPERATIONS.md](OPERATIONS.md), a coherent
Agent-C daemon/client revision and authenticated access. Applying policy requires
`--tools host --host-policy full`; read-only preview remains available under a
read-only host policy. The default configuration leaves all limits and the
background interval at zero, disabling them.

1. Open **Memory → Advanced → Retention & cleanup** and preview the subject
   limits. This illustrative request body can also be sent through an existing
   authenticated API client:

   ```json
   {
     "dry_run": true,
     "daily_max_days": 30,
     "daily_max_bytes": 10485760,
     "checkpoint_max_days": 30,
     "checkpoint_max_count": 200,
     "structured_deprecate_days": 90,
     "structured_deprecate_max_entries": 50
   }
   ```

   Units are days, raw active-file bytes and counts. Here `10485760` is
   `10*1024*1024` bytes (10 MiB). Zero disables an individual bound.
   `daily_max_days` is 0–36,500; other limits must be nonnegative.
2. Inspect the versioned `agent.memory.retention.v2` report. `*_selected_count`
   shows planned work. During preview, `*_deleted_count` and
   `structured_deprecated_count` remain zero; `outcomes[].commit_state="preview"`
   is not a publication receipt. `daily_bytes_projected` estimates the selected
   policy's result, while `daily_bytes_after` still equals captured active bytes.
   Preview creates no locks, archives, receipts or changed facts on disk.
3. Apply the reviewed policy (`dry_run=false`). Execution captures again; preview
   does not authorize stale revision reuse. A retirement receipt
   (`agent.memory.retirement.v1`) records path/layer, source ID,
   `expected_revision`, exact whole-source `original_evidence`, commit state and
   an observed `current_revision` when available. `head_removed` describes this
   attempt. Confirmed success is `retired`, current revision `absent`, with the
   synced original retained.
4. Check `ok` and `complete`. HTTP `200` means the selected bounded pass completed.
   HTTP `409` returns the same full report when work is incomplete. Successful
   independent retirements remain committed. The UI retains that report, shows
   **Cleanup incomplete** and the confirmed counts/errors, and keeps raw receipts
   collapsed. It does not retry automatically.
5. Recover from the specific state below. Re-read active heads and exact original
   evidence before deciding on another action. Keep independent requirements
   runnable. A new pass derives new decisions; it must not reuse a stale removal
   decision or resubmit an unrelated external effect.

| Receipt/status | Observation and next action |
| --- | --- |
| `retired` | The active name was removed and its parent synced. Inspect the exact original through the returned evidence reference. |
| `revision_conflict` | This attempt did not remove the captured head. Another revision or absence was observed. Read and reconsider policy against current bytes. |
| `busy` / `cancelled` | No removal by this attempt. Respect cancellation; investigate a persistent lock before another bounded pass. |
| `not_retired` | Original retention, source validation or removal failed. The error identifies the storage/path cause. Repair it and inspect the current head. Counters do not claim success. |
| `retired_unconfirmed` | Removal occurred but directory durability/acknowledgment was interrupted. Verify current absence and the retained original. Do not blindly replay or treat this as a confirmed retirement. |
| `deferred_count>0` / `budget_exhausted` | Some work was not attempted. Preserve completed receipts and perform another bounded capture when eligible. |

`daily_bytes_before/after` account for the captured readable sources minus only
confirmed retirements. Unconfirmed removals are conservatively charged as
remaining; concurrent later writes require a new capture. These fields do not
measure free disk space. The report preserves errors (up to 50, with
`errors_dropped` for overflow), per-source outcomes and the actual
`structured_write_response` when a deprecation write is attempted.

Structured age policy reads authoritative `STRUCTURED.md`. It uses stored
`observed_utc`, or explicitly falls back to stored `updated_utc` when observation
time is missing/empty. An ill-typed observation field is reported and skipped;
malformed calendar strings are also reported and skipped. No read-time migration
creates an observation date. These metadata are not proof of an event date. One
revision-checked batch changes only eligible string-valued records, bounded to
200 entries (default 50). Remaining candidates stay pending; confirmed key
lists/counts remain separate from selection.

### Bounds, pruning and verification

A pass captures at most 4,096 sources and 64 MiB total, with the store's 8 MiB
per-source limit. Each directory walk permits at most 4,096 entries. Exceeding a
hard capture budget stops before any retirement. Ordinary per-source read/parse
errors remain visible while independently eligible sources can proceed. At most
128 file retirements plus one bounded structured batch run per pass. A shared
10-second cooperative deadline and cancellation apply to capture, lock waits and
mutation boundaries. Kernel filesystem I/O can outlast that deadline; this is
not a real-time guarantee. Each root lock wait retains the store's five-second
bound. Background stop uses the same cancellation port and atomic stop state;
failures are reported by the existing engine instead of being silently counted.

`memory_put` auxiliary checkpoint pruning uses its already-held writer lock.
It retains each pruned original through the same retirement implementation and
cannot retire its primary source or alter the primary commit receipt. Its
separate best-effort policy keeps recent checkpoint file modification times,
scans at most 4,096 directory entries and retires at most 50 files within a
cooperative two-second maintenance window. `keep_checkpoints` is clamped to
1–1,000 for active checkpoints. `checkpoint_prune_ok` and
`checkpoint_pruned_count` expose its result. An incomplete scan does not choose
pruning candidates. Maintenance failure cannot revoke a committed fact update.

The existing configuration fields select scheduling and policy:
`memory_retention_interval_ms`, `memory_retention_daily_max_days`,
`memory_retention_daily_max_bytes`, `memory_retention_checkpoint_max_days`,
`memory_retention_checkpoint_max_count`, `memory_retention_structured_deprecate_days`
and `memory_retention_structured_deprecate_max_entries`. The broker's existing
`POST /v1/agents/{agent_id}/memory/retention/enforce` forwards to deployment policy;
this milestone does not enable any deployment or broker fan-out.

From the repository root on the Linux development host:

```sh
cmake -S . -B build-snapshot -DCMAKE_BUILD_TYPE=Debug -DAGENT_BUILD_HOST=ON -DAGENTD_ENABLE_HTTP=ON
cmake --build build-snapshot --target memory_retirement_tests memory_store_tests memory_promotion_tests memory_view_tests host_toolset_tests agentd -j4
ctest --test-dir build-snapshot -R '^(memory_retirement_tests|memory_store_tests|memory_promotion_tests|memory_view_tests|host_toolset_tests|agentd_memory_retention_smoke|agentd_memory_consolidate_smoke)$' --output-on-failure
```

Pass requires every selected check to succeed. Fixtures qualify exact original
retrieval after removal, empty files, newest-checkpoint selection, stale/current
revision conflicts, original archive failure, independent partial results,
uncertain outcomes, cancellation before/after archival, read-only preview/apply,
128-operation and 64-MiB capture limits, strict dates, linked/non-normalized
sources, current FTS/fallback invalidation, root-lock reuse and competing real
process writers. Actual `SIGKILL` immediately before/after unlink verifies process
restart boundaries. The HTTP fixture verifies archive bytes against returned
SHA-256/span receipts and an incomplete response with an independent success.
Browser coverage checks the retained partial report and closed metadata. These
are synthetic Linux checks; Windows runtime, storage power loss, total disk
reclamation, MCU execution and live-provider behavior are unqualified. On another
host, repeat these checks and qualify its filesystem's unlink/fsync semantics.

## Explicit archive expiry

Archive expiry removes old original **payloads**, so it is separate from retiring
an active head. `archive_max_days=0` is the default and preserves history.
Ordinary writes no longer perform the previous silent count-based pruning. That
old implementation selected from a partial directory scan; the replacement first
captures a complete bounded inventory or stops without expiry. This source change
was exercised only in temporary fixtures. Production remains stopped.

### Scope and policy

[`memory_archives.cpp`](../cli/src/memory_archives.cpp) reuses the root writer lock
and atomic metadata publication from [`memory_store.cpp`](../cli/src/memory_store.cpp).
It is a host implementation; the portable C kernel is unchanged. The existing
`POST /api/v1/memory/retention/enforce` accepts `archive_max_days` in 0–36,500.
For example, these are illustrative request bodies, not instructions to restart
or call the stopped production service:

```json
{"dry_run":true,"archive_max_days":30}
```

Inspect `archive_expiry.selected`, `protected_count`, `outcomes`, `errors` and
`complete` first. To apply an authorized policy on a running development/deployed
instance, submit the same positive age with `dry_run=false`. This permanently
unlinks qualifying originals. A preview is a plan, not a saved execution grant;
apply recaptures state under the writer lock. Saving required references first is
essential. The WebUI keeps this option under **Retention & cleanup → Expire old
history**, starts at zero and retains the normal preview default.

Age is measured from each archive file's filesystem modification time:

`eligible age = mtime < capture_time − archive_max_days × 86,400 seconds`.

Equality is retained. This is archive age, not publication/event/observation time.
No count quota or physical free-space guarantee is implied. The explicit API
request and native policy select expiry; the existing periodic retention engine
does not enable this new policy automatically. Its ordinary retirement schedule
and production configuration are unchanged.

### Protect evidence before expiry

Under the same root lock used by cooperating writers, apply captures active
public `.md`, `.markdown` and `.json` documents beneath the memory root. It excludes
hidden cache/lock/expiry/revision directories. The exact hash of each active head
is protected. Every literal lowercase `sha256:<64 hexadecimal digits>` in those
active documents also protects that content revision across archive buckets.
This deliberately conservative rule covers structured versions, promotion
receipts, recap manifests and raw Markdown references without guessing their
schema or interpreting their claims. Identical content at another path may keep
an otherwise unused archive protected. A combined retirement/expiry pass also protects every original revision named in its own newly issued retirement receipts, so the response cannot immediately invalidate those references. Native callers can supply the same bounded pass-local receipt pins; longer retention still requires an active document reference. `protected_count` counts archive objects
protected this way, including objects too recent for the age cutoff.

External databases, workflow receipts or files outside this memory root are not
inventoried by this policy. Before enabling expiry, preserve any original they
still require by putting its complete immutable evidence reference (or its exact
`sha256:` revision) in an active memory document through the normal tools. For
example, append `Required original: sha256:<actual-original-hash>` to a maintained
memory note. This is a policy pin, not proof of the referenced claim. Removing the
last active reference permits age-based expiry immediately if the archive is already old; it never executes or
replays the action that produced the evidence. This is scoped archival retention,
not a universal cross-system forget or privacy-erasure guarantee.

### Publication and recovery

1. Capture active references and a complete archive inventory. Select only regular
   `.revisions/<path-sha256>/<raw-content-sha256>.blob` files. Symlinks are not
   followed. Unknown filenames are preserved. Sort eligible unprotected objects
   by oldest mtime, then relative path.
2. Before each selected unlink, read exact bytes and compare SHA-256 with the
   filename/revision. Corrupt, changed or missing objects are preserved/reported;
   another eligible original can still progress. The file-size and hash both
   refer to the complete original, not a snippet.
3. Atomically publish a small immutable intent at
   `.expiry/<path-sha256>/<raw-content-sha256>.json`. It contains schema
   `agent.memory.expiry_intent.v1`, `state=prepared`, archive path, revision,
   byte count and the policy age. It contains no original payload. A missing,
   malformed, oversized or conflicting intent prevents that object's unlink.
4. Recheck the original revision, check cancellation, unlink and sync its parent
   directory. The original head and its primary transaction receipt are not
   changed. Existing intents can be reused only after fresh eligibility checks;
   an intent never authorizes deleting a now-protected or recent archive.
5. Check `commit_state`: `expired` means unlink and directory sync completed;
   `expired_unconfirmed` means removal occurred but durability/acknowledgment was
   interrupted; `not_expired` preserves the error; `preview` performs no write.
   `expired`/`expired_bytes` count confirmed removals only. These byte counts do
   not measure reclaimed physical disk space. `uncertain` and `deferred` remain
   separate, and HTTP 409 preserves the complete cleanup report.

An intent proves preparation, **not completion**. After a crash, inspect the
original path/hash and its intent. If it still exists, a fresh bounded pass can
re-evaluate the policy. If absent, do not count another expiry or substitute the
current source. Exact evidence reads fail explicitly when the original is no
longer retained. No model/provider or external effect is replayed by expiry.
Intents remain as metadata even after payload removal. Storage power-loss behavior
and forensic SSD/snapshot/backup erasure are not qualified by these host tests.

### Operating bounds and verification

Each active-document, archive and intent inventory is capped at 4,096 directory
entries. Active document traversal is capped at depth 8; each source is at most
8 MiB. Captured active bytes plus selected original bytes are capped at 64 MiB,
with at most 20,000 protected revisions. A hard capture-cap or enumeration failure
stops before unlink. Invalid archive objects remain preserved/reported while independent objects may progress. Revision rechecks add reads; this is a capture-memory bound,
not a total I/O byte ceiling. A cooperative 10-second deadline, the existing
five-second writer-lock limit and cancellation cover the pass; kernel I/O can
outlast a cooperative deadline.

At most 128 originals are attempted, sharing the existing retention pass's
128-file mutation allowance. Remaining selected work is explicit. At most 2,048
new/existing intent records are admitted per root; existing matching intents can
still reconcile at that cap. The metadata inventory has its own 4,096-entry cap.
Errors are capped at 50, with `errors_dropped` for overflow; each attempted object retains its own outcome. Capacity exhaustion preserves originals and returns incomplete; expiry-intent
metadata compaction remains future work. [Promotion receipt compaction](#compacted-receipts-and-recovery)
is separate and preserves consumed IDs and original revision pins. This policy does not delete
metadata history to make room automatically.

From the repository root on the assertion-enabled Linux host build:

```sh
cmake -S . -B build-snapshot -DCMAKE_BUILD_TYPE=Debug -DAGENT_BUILD_HOST=ON -DAGENTD_ENABLE_HTTP=ON
cmake --build build-snapshot --target memory_archive_tests memory_store_tests agentd -j4
ctest --test-dir build-snapshot -R '^(memory_archive_tests|memory_store_tests|memory_retrieval_smoke|agentd_memory_retention_smoke)$' --output-on-failure
```

Pass requires all selected checks to succeed. The fixtures cover preservation
across 130 ordinary corrections, active raw/recap references, explicit expiry,
metadata intents, corruption, linked paths, changed captured heads, cancellation,
operation/byte/inventory/intent caps and readonly policy. Competing processes and
actual `SIGKILL` before/after unlink verify local restart visibility and no duplicate
expiry count. The temporary daemon tests preview/apply, strict age types and
unchanged live content without another provider call. Browser checks cover the
closed zero-default control, confirmed versus uncertain counts and collapsed
receipts on a 320-pixel screen. These checks do not qualify MCU execution, Windows
runtime, all external consumers, irreversible storage sanitization or retrieval
quality.
The [archive qualification receipt](verification/memory-archive-expiry-20261120.json) records the tested revision, artifact hashes and measurements.

## Memory recaps (LLM summaries)

Recaps are generated interpretations, not authoritative facts. `POST
/api/v1/memory/recaps` selects current salience inputs. `dry_run=true` previews
whole inputs without calling a model or saving originals. The configured
`summary_model` is used unless the request supplies `model`/`summary_model`.
Saving requires `tools=host` and Full host policy. No production model or schedule
is enabled by this source change.

### Derive and preserve an outcome

[`memory_recaps.cpp`](../daemon/src/memory_recaps.cpp) uses
[`memory_derivation.cpp`](../cli/src/memory_derivation.cpp) for reusable provenance:

1. Capture current inputs and choose whole item blocks within a 12,000-byte
   prompt. Omitted items produce `input_incomplete`/`dropped_inputs`; IDs and
   UTF-8 are not cut mid-item. The selected evidence includes supporting marker
   originals. Provenance above 6 MiB stops before a model call.
2. Save `derivation` with schema `agent.memory.derivation.v1`, policy
   `agent.memory.view.v1`, current-view UTC cutoff, exact input path/source/revision
   rows and immutable evidence references. Under the existing writer lock, retain
   actual captured originals before the provider call when saving is requested.
3. Make at most one provider request: no retry/backoff or proxy-to-direct replay,
   at most 60 seconds, default 1,024 output tokens and maximum 2,048. Non-streaming
   response capture stops at 1 MiB. Cancellation is shared with the caller/engine.
   These are limits, not a claim that the provider honors token requests.
4. Keep the exact returned assistant text, including token-limited partial output, as `model_response`. Token-limit/provider errors keep generation incomplete. `summary_text` is
   a UTF-8-safe display excerpt of 200–6,000 bytes, selected by the existing
   `summary_max_chars` request setting (its unit is bytes). Record actual returned
   model/response ID and token usage when present, separately from requested
   model. `response_capture_sha256` hashes captured response-body bytes;
   `response_capture_complete=false` means the hash covers only the captured
   prefix, not a complete provider result. Oversize/transport uncertainty does
   not become a completed result or trigger another call.
5. Recheck input revisions after the response and under the publication writer
   lock. Save an immutable `agent.memory.recap.v2` artifact at
   `recaps/recap_<opaque-id>.json` through the existing atomic store. The opaque
   name is not a content hash; `recap_revision` hashes its exact final bytes.
   Publication retains original assistant text and the manifest once. Item rows
   refer to evidence IDs rather than copying complete references again.
6. On changed inputs, empty output or an incomplete provider result, preserve a
   historical artifact with `generation_status=incomplete`. HTTP 409 retains the
   full report and any published original-response reference. Directory-sync
   uncertainty remains `publication_uncertain`; inspect the original path/hash
   before more work. A local publication failure retains the outcome in the
   returned report. Never blindly replay the provider request.

This path does not yet have a durable **pre-provider admission**. Process death
before its local outcome receipt, or loss of an unsaved HTTP report, can leave an
unknown provider outcome. That lifecycle gap remains on the roadmap. Completed
workflow model journals have a separate, stronger recovery contract.

### Read current summaries or inspect history

`GET /api/v1/memory/recaps` defaults to `view=current`. Each list request recaptures
manifest inputs once, checks exact source IDs/revisions and requires every
reference to remain eligible with the same complete reference value. A changed
source conservatively invalidates the summary even if its edited passage was
not selected. Missing/unsupported manifests, altered spans, wrong scopes and
incomplete generation cannot enter current output. `source_state` distinguishes
`current`, `stale`, `unverified` and `unavailable`; `claims_verified` is always
false. `inputs_current=true` means provenance still matches, not that generated
claims are supported.

Use `view=historical` or `view=evidence` to inspect identified originals, including
older unversioned summaries. The response returns exact revision/span references
for `/summary_text` and `/model_response` when those fields exist. It validates
input and artifact revisions again before output. No list/invalidation operation
calls a model or replays another external effect. Current automatic context still
excludes recaps until claim support is validated.

Listing captures at most 128 artifacts, 64 MiB total and 4,096 directory entries.
Manifest inputs share the retrieval limit of 128 source paths; each manifest
permits 20,000 unique evidence references. Exceeding a hard capture cap fails
explicitly. Output `limit` is 1–200 and excess eligible rows set `incomplete`.
Recap head retirement remains necessary before the 128-artifact list cap; archive payload expiry does not reduce the active recap inventory. `kind=daily`
is an optional list filter; absent kind is treated as `manual`.

Scheduled recaps are disabled by default. Existing daily/weekly interval and
lookback settings still select this same path. A failed attempt advances its
in-process interval; an existing incomplete saved result seeds the next interval
after restart. This prevents immediate replay, but is not durable admission or
a guarantee against all crash-window duplication. Engine stop cancels in-flight
provider work and leaves original outcome publication eligible to complete.

### Reproduce derived-state checks

From the repository root on the assertion-enabled Linux host build:

```sh
cmake -S . -B build-snapshot -DCMAKE_BUILD_TYPE=Debug -DAGENT_BUILD_HOST=ON -DAGENTD_ENABLE_HTTP=ON
cmake --build build-snapshot --target memory_cache_tests agentd -j4
ctest --test-dir build-snapshot -R '^(memory_cache_tests|memory_retrieval_smoke|agentd_memory_correlate_smoke|agentd_workflow_memory_correlate_smoke|agentd_memory_recaps_smoke)$' --output-on-failure
```

Pass requires every selected check to succeed. The cache fixture proves immediate
registered-file removal before another query, sidecar ownership, current rebuild
scrubbing, bounded contention/fallback, source preservation on failure, exact
originals and derivation invalidation. The temporary daemon fixture makes five
synthetic loopback calls: completed result, concurrent correction, 2-MiB response
HTTP 503 and a token-limited partial completion. It checks immutable outcomes, current/history filtering, the 1-MiB
capture digest and no provider replay. Browser checks retain HTTP 409 results,
label changed sources and keep receipts closed. The [qualification receipt](verification/memory-derived-state-20261120.json) pins the tested source and artifacts. These checks do not establish
semantic answer accuracy, physical storage erasure, production deployment, MCU
qualification or paid retrieval improvement.

## API helpers (correlation)

`GET /api/v1/memory/correlate?trace_id=...` defaults to current view. It captures
eligible structured heads and recent daily observations, matches exact
`trace:<id>` or colon-delimited child tokens and validates revisions before
output. `trace:alpha-other` does not match `trace:alpha`. `structured_path` and
`key_prefix` restrict candidates before the shared structured/daily result cap; excerpts remain bounded
and original references preserve full values/relationships. `index.engine` is
`live_projection`; no correlation payload is copied into a second SQLite/JSON
cache. Recaps do not participate in this join.

Explicit `view=historical`/`view=evidence` selects immutable checkpoint originals.
Time windows and `timeline=true` require one of those views. The native workflow
helper uses the same implementation; `/api/v1/trace` requests history when a
memory time window/timeline is supplied. Checkpoint listing remains available at
`GET /api/v1/memory/checkpoints` with optional `structured_path`.

`POST /api/v1/memory/correlation/index` now inspects/counts the current projection;
its request accepts `daily_days` (0–90), `include_structured` and `include_daily`.
It creates no payload cache and is not a prerequisite for correlation. There is
no automatic correlation rebuild after consolidation or recap generation. The
response graph (`agentd.memory.relationship_graph.v1`) is a bounded navigation
view of declared trace/workflow/task relationships, not claim verification.

## WebUI Memory Explorer

The WebUI exposes a **Memory explorer** panel (collapsible) that directly calls the memory endpoints:

- Structured query (`/api/v1/memory/query`)
- Trace correlation (`/api/v1/memory/correlate`)
- Current correlation inspection (`POST /api/v1/memory/correlation/index`)
- Checkpoint listing (`/api/v1/memory/checkpoints`)

This panel is intended for operator/debug use and returns raw JSON so you can inspect structured memory state
and evidence hashes without leaving the UI.

## Deterministic workflow tasks

Durable workflows can query memory without invoking an LLM:

- `kind:"memory_correlate"` — bounded current correlation by `trace_id`, with explicit historical checkpoint views; results include the same `relationship_graph` shape as `/api/v1/memory/correlate`
- `kind:"memory_query"` — bounded query of the structured **current view** by `key_prefix` (authoritative head by default; explicit historical/evidence windows select checkpoints)

## Remaining authority work

The current view removes explicitly stale candidates and invalidated marker-derived
heads; durable promotion receipts prevent old observations from undoing later
corrections. This does not finish general correction/forget semantics. Daily/checkpoint retirement now retains originals under the declared
[retirement policy](#source-retirement-and-retention); explicit archive expiry is described above. Registered cache purge and recap provenance checks are implemented above. Generic conclusion validation, bounded hard forgetting and
provenance-preserving handoffs remain open. Invalidating evidence must never
resubmit a completed external effect. Keep these requirements ahead of the paid
retrieval-quality pilot; do not interpret green eligibility tests as completion
of the broader memory-authority goal.
