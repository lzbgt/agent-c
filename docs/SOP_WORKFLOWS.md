# SOP workflows v1

`POST /api/v1/workflow/submit` accepts exactly one of the existing `tasks` array, an inline `sop` object, or an exact saved `sop_ref`. The server compiles SOP stages into the existing durable DAG before ordinary task validation, policy enforcement, admission control, persistence and scheduling. The WebUI workflow composer includes **SOP · stages and actions** as a JSON template.

```json
{
  "sop": {
    "schema": "agentd.sop.v1",
    "id": "inspection",
    "version": "1",
    "title": "Inspection process",
    "stages": [
      {"id": "intake", "actions": [{"task_id": "register", "kind": "delay", "delay_ms": 0}]},
      {"id": "checks", "mode": "parallel", "actions": [
        {"task_id": "quality", "kind": "delay", "delay_ms": 0},
        {"task_id": "inventory", "kind": "delay", "delay_ms": 0}
      ]},
      {"id": "release", "actions": [{"task_id": "record", "kind": "delay", "delay_ms": 0}]}
    ]
  }
}
```

This provider-free fixture demonstrates ordering only. Replace the delay actions with real validated workflow tasks for application work; it does not perform an inspection. The exact file is [sop_inspection.json](examples/workflows/sop_inspection.json).

Stage order is significant. Each stage waits for successful completion of the previous stage. Within a stage, `sequence` (default) chains actions; `parallel` gives all actions the same incoming dependencies and joins them before the next stage. Existing task `expect` checks and configured retries still apply. Failure does not release later stages.

IDs and version strings use `A-Za-z0-9_.:-`, 1–128 bytes. Stage IDs are unique; action `task_id` values are globally unique across the SOP and remain unchanged in task references. Titles are optional, up to 256 bytes. Bounds are 32 nonempty stages and 128 total actions. The compiler rejects unknown SOP/stage/action fields, explicit action dependencies, `allow_error=true`, dependency inference, unsupported kinds and submit-time parallel macros. This prevents a misspelled control rule from being silently ignored.

Actions use the existing task payload contracts. Supported kinds are `llm` (or an omitted kind with `request`), `delay`, `decision`, `host_tool`, `http_json`, `agentd_call`, `aggregate`, `avm_capsule`, memory task kinds, `experience_record`, `edge_invoke`, `edge_wait_sensor`, and sequential `delegate`. Native HTTP/device/capsule policies remain effective; SOP input does not grant capabilities. See [WORKFLOWS.md](WORKFLOWS.md) and the generated OpenAPI contract for their payloads.

The stored diagnostic spec contains the original `sop`, compiled `tasks`, and server-owned `sop_compiled` metadata: compiler schema, definition ID/version, canonical definition SHA-256 and stage-to-task mapping. The hash uses `agent_json_c14n_v1`, not an unspecified JSON serialization. It identifies the submitted definition, not externally supplied defaults or secrets; the entire redacted run spec records those execution settings separately. Inline SOPs remain ad hoc: reusing an ID/version with changed inline source produces a different hash. Saved versions enforce immutability as described below.

Submission idempotency retains the existing first-accepted-wins semantics. It does not guarantee exactly-once external effects. New persisted specs above 256 KiB are rejected instead of being truncated into invalid JSON. For a fresh SOP submission, submit its `sop` and desired workflow-level settings, omitting server metadata and generated tasks. For a deliberate DAG copy, submit just its compiled tasks/settings. Diagnostic `spec` is not itself a ready-to-resubmit request.

## Explicit review steps

A `decision` action durably waits for one configured continue/fail choice without holding a worker, consuming an attempt or reserving execution budget. Independent work continues. The UI's **SOP · review decision** template and **Process decisions** run card provide an explicit answer/receipt flow. Stops, expiry, exact replay and saved-version identity are preserved. This is a process choice under existing daemon authentication, not a per-person authorization grant. See [WORKFLOW_DECISIONS.md](WORKFLOW_DECISIONS.md).

## Saved SOP library

The composer exposes **Saved SOP versions**. **Save SOP version** saves only the editor's inline `sop`, without starting a workflow. **Use version** replaces the editor with an exact hash-pinned reference; review/add workflow inputs, defaults and limits, then explicitly submit to run.

`POST /api/v1/workflow/sops` accepts `{"sop": {...}}` (maximum 128 KiB request) and structurally compiles the definition before saving. The daemon-local SQLite registry is append-only: a new `(id, version)` returns 201; identical canonical content returns 200 with `created:false`; changed content at that identity returns 409. Save a new version to edit. There is no overwrite/delete endpoint and no implicit “latest” selection. Version strings are opaque, not a semantic-version ordering.

`GET /api/v1/workflow/sops` lists metadata newest first, without source bodies. Use `limit=1..100` (default 25) and the returned exclusive `next_before_id` cursor for older pages. `GET /api/v1/workflow/sops?id=inspection&version=1` returns the exact definition, including its `sop`.

Submit a saved version through the existing workflow API:

```json
{"sop_ref": {"id": "inspection", "version": "1"}, "inputs": {"batch": "example"}}
```

An optional `sop_ref.definition_hash` asserts the expected canonical content; mismatch is 409, missing version is 404. The composer always supplies it. The server verifies saved source integrity, copies the definition into the run, then performs normal compilation, task validation, policy and admission checks. The persisted `sop_ref` always includes the resolved hash. Saving is not execution approval or proof that every task payload is valid. A new saved version cannot change an existing run, even after restart; owner cancellation/deadlines remain effective.

The library uses the same daemon authentication/access boundary as other workflow APIs. It is not a secret store. Known inline credential fields (including API keys, authorization headers, passwords and access/refresh tokens) are rejected rather than silently redacted into unusable execution source. Use server-side credential references. This check is not a general secret detector; never place credentials in free-form prompts, command strings, URLs or arbitrary data fields. Source is returned only on an exact read/save, not in metadata pages. Keep database backups and authorized erasure under the existing daemon-state policy.

SOP v1 is available on direct workflow submission. Schedule/runtime-skill authoring may still require a tasks-based bundle; the saved library does not add schedule/runtime-skill authoring support. General external signals, person/role-bound process approvals, branches, loops, compensation and active-run version migrations are explicitly future work in [the architecture](ARCHITECTURE_CURRENT.md). The independent [work ledger](AUTONOMOUS_WORK.md) remains separate from the scheduler-level decision node.
