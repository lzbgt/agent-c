# SOP workflows v1

## Guided process authoring

Select the SOP template to name and version a process, reorder stages/actions, and choose sequential or parallel execution. Add model requests, relative waits, fixed UTC waits, external signal waits or process decisions. Edits preserve advanced action settings; editing, adding and reordering never submit or save automatically.

Decisions expose the question, dependency-relative expiry (0 for indefinite, otherwise up to 30 days), and 1–8 choices with explicit IDs, labels and continue/fail outcomes. New choices default to fail, not implicit approval. Choices are process inputs, not person/role authorization or tool permission. Ordinary server validation remains authoritative.

**Wait until a UTC deadline** stores an exact millisecond timestamp. The control is explicitly UTC even in a non-UTC browser; it does not schedule local-time recurrences. New waits default to one hour after authoring. Saved SOPs preserve the same instant across runs, and a past-due wait completes after dependencies finish. Clearing the deadline leaves an invalid draft visibly marked rather than silently choosing another time.

The browser regression in `ui/e2e/workflow_sop_editor.spec.ts` requires `build/agentd` (build it with `cmake --build build --target agentd`). It passes the exact browser-authored JSON to `tests/workflow_authored_process_smoke.py`, which uses a temporary database and loopback daemon to save/pin/run it and verify configured continue/fail choices and idempotent delivery. No real provider or production daemon is used.

## Guided integration reads

Add **Read integration JSON (GET)** to fetch data through the existing HTTP workflow runtime. Configure the URL, optional name of an already-configured server bearer credential, timeout (1–300000 ms) and response bound (1024–16777216 bytes). New drafts have a blank URL, 30-second timeout and 1 MiB bound; editing does not contact the destination. Save and submit remain explicit.

Do not paste credentials into the URL or free-form source. Credential references do not create credentials or enable HTTP tasks. Server destination policy and the existing HTTP-task enablement still apply. GET can transmit its URL and configured credentials; choose the intended destination deliberately. Existing advanced headers, retries and other action settings are preserved. POST/effect configurations remain advanced and are never silently converted to reads.

`ui/e2e/workflow_sop_http.spec.ts` passes the exact browser-authored source to `tests/workflow_authored_http_smoke.py`: a temporary authenticated daemon saves the SOP without execution, pins the saved version and performs one synthetic loopback GET with preserved headers and a server-only credential reference. The browser also verifies mobile width, no implicit submission and POST/effect preservation. No production server or external provider is involved.

## Runtime contract

`POST /api/v1/workflow/submit` accepts exactly one of the existing `tasks` array, an inline `sop` object, or an exact saved `sop_ref`. The server compiles SOP stages into the existing durable DAG before ordinary task validation, policy enforcement, admission control, persistence and scheduling. The WebUI workflow composer includes **SOP · stages and actions** with a guided process builder; raw JSON remains under **Advanced process configuration**.

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

IDs and version strings use `A-Za-z0-9_.:-`, 1–128 bytes. Stage IDs are unique; action `task_id` values are globally unique across the SOP and remain unchanged when repeat_count is omitted or one. Titles are optional, up to 256 bytes. Bounds are 32 nonempty stages and 128 total expanded actions, including repetitions. The compiler rejects unknown SOP/stage/action fields, explicit action dependencies, `allow_error=true`, dependency inference, unsupported kinds and submit-time parallel macros. This prevents a misspelled control rule from being silently ignored.

### Guided host commands

Choose **Run a host command** to author a deterministic `proc_exec` action. The executable and every argument have separate fields; spaces, metacharacters and empty arguments are preserved, not split or passed through a newly added shell. A shell executable can still interpret its own arguments. Set a bounded command timeout. Existing advanced argument/settings fields are preserved; other host tools stay advanced.

Editing and saving never execute commands or grant capabilities. The daemon must already allow host process execution; its policy, approval and budget checks still apply. Do not embed secrets in persisted arguments. An interrupted command can have partial effects: inspect the original outcome instead of copying the process as a blind retry. See [guarded host effects](WORKFLOW_EFFECTS.md).

`ui/e2e/workflow_sop_host.spec.ts` executes the exact browser-authored saved/pinned SOP using `tests/workflow_authored_host_smoke.py`. Isolated daemons verify literal argv output with one completed journal receipt, and disabled execution policy with zero admitted effects. Browser checks cover add/remove arguments, empty strings, advanced field/tool preservation, mobile layout and no implicit submission.

Actions use the existing task payload contracts. Supported kinds are `llm` (or an omitted kind with `request`), `delay`, `timer`, `decision`, `signal`, `host_tool`, `http_json`, `agentd_call`, `aggregate`, `avm_capsule`, memory task kinds, `experience_record`, `edge_invoke`, `edge_wait_sensor`, and sequential `delegate`. Native HTTP/device/capsule policies remain effective; SOP input does not grant capabilities. See [WORKFLOWS.md](WORKFLOWS.md) and the generated OpenAPI contract for their payloads.

The stored diagnostic spec contains the original `sop`, compiled `tasks`, and server-owned `sop_compiled` metadata: compiler schema, definition ID/version, canonical definition SHA-256 and stage-to-task mapping. The hash uses `agent_json_c14n_v1`, not an unspecified JSON serialization. It identifies the submitted definition, not externally supplied defaults or secrets; the entire redacted run spec records those execution settings separately. Inline SOPs remain ad hoc: reusing an ID/version with changed inline source produces a different hash. Saved versions enforce immutability as described below.

Submission idempotency retains the existing first-accepted-wins semantics. It does not guarantee exactly-once external effects. New persisted specs above 256 KiB are rejected instead of being truncated into invalid JSON. For a fresh SOP submission, submit its `sop` and desired workflow-level settings, omitting server metadata and generated tasks. For a deliberate DAG copy, submit just its compiled tasks/settings. Diagnostic `spec` is not itself a ready-to-resubmit request.

## Conditional actions

Opt in with **Run only when a previous result matches** on an action. Select an earlier action, a result field expressed as a JSON pointer, and an exact Boolean, text or explicit-null value. Editing never evaluates the action or grants permission; saving remains non-executing and ordinary server validation is authoritative. Reordering preserves the selected reference and visibly marks it invalid if it is no longer an earlier action.

The same top-level `when` field is accepted on individual raw DAG tasks:

```json
{
  "task_id": "optional_check",
  "kind": "delay",
  "when": {"task_id": "external_input", "pointer": "/payload/accepted", "equals": true},
  "delay_ms": 0
}
```

This example performs no real inspection. A signal's delivered object is under `/payload`; choose a field from the actual result contract of the source action. The source dependency is added automatically. Raw DAG admission rejects missing references, self references and cycles. SOP references name an earlier stage's source action or an earlier action in the same sequential stage, never a peer in the same parallel stage. With repeated stages, the compiler pins an earlier same-iteration action, or the latest completed repetition from a prior stage, to its exact expanded task ID. Other advanced input/template references are not rewritten.

Conditions deliberately support only same-type scalar equality: Boolean, string up to 1024 UTF-8 bytes without NUL, or explicit JSON null. Numbers, compound values, expressions, coercions and template expansion are not supported. The nonempty pointer is bounded to 512 bytes and 32 segments, uses strict `~0`/`~1` escaping, and accepts only canonical nonnegative array indices. Missing pointers are distinct from present nulls. Unknown condition fields, `defaults.when`, `request.when`, guards inside delegate attempts, guarded submit-time parallel macros and guarded `allow_error=true` tasks are rejected.

The runtime checks the successful, non-skipped source result before templates, waits, model/tool dispatch and execution budget/effect admission:
- A match executes the ordinary action under unchanged policy, approvals, budgets, retry and recovery rules.
- A non-match persists a successful **skipped** receipt. It does not execute the action, arm a wait, reserve execution resources or emit an external effect. Execution usage is zero; the normal task claim/attempt counter still records the condition evaluation. Action-specific `expect` checks apply only when the action executes.
- A missing, failed, skipped or wrong-typed source result fails closed, never taking a default/opposite branch. Missing/type errors are terminal and are not retried. A failed dependency follows ordinary DAG blocking semantics.

Skipped actions retain scheduler status `done` for dependency joins. Full results carry `skipped:true` and a bounded `agentd.workflow.condition.v1` receipt containing the exact test, actual value and match result. Default task reads expose only skip status and source/pointer/match metadata, not compared values. The UI separates executed and skipped counts and keeps technical condition details collapsed. A downstream action without its own condition still runs after a skipped predecessor; branching is per action, not an implicit whole-stage jump.

Conditions add no tool capability, person/role authorization, new scheduler or database schema. They are bounded optional actions, not a general expression/transition engine, dynamic loop, compensation mechanism or distributed exactly-once guarantee. `tests/workflow_condition_smoke.py` exercises real isolated daemon effects, restart, cancellation, budget/policy refusal and repeated saved SOPs. `ui/e2e/workflow_conditions.spec.ts` passes its exact browser-authored definition to that same native harness using `build-snapshot/agentd`.

## Repeated stages and run progress

Set a stage's optional `repeat_count` to 1–32 for a fixed number of intentional repetitions. Each iteration waits for the prior iteration's successful terminal actions. This is not a retry or a conditional loop: commands and writes intentionally happen again. Normal policy, cancellation, deadlines, budgets and uncertain-effect guards remain effective.

For counts greater than one, task IDs become `source_id:repeat:N` (one-based). Expanded IDs must remain globally unique and fit 128 bytes. Only `when.task_id` source references are rewritten to their current/prior expanded action as described above; other advanced references must use exact expanded IDs. All expanded tasks count against the 128-task bound. The stored manifest includes each iteration's task IDs; restart resumes those persisted tasks, not a new loop.

Enable **include spec** in the advanced workflow controls and reload to load the process mapping. The read-only **Process progress** card groups last-loaded task states by stored stage and iteration. Counts are completed tasks, not elapsed-time percentages or acceptance guarantees. When conditions skip work, executed and skipped counts are shown separately; skipped actions were not executed. Missing rows show “not loaded”; explicit blockers show “needs attention”; queued tasks are not claimed to be executing. Reload uses the existing workflow read. There is no implicit retry, submit, approval or effect reconciliation.

## External-input steps

A `signal` action waits for one explicitly delivered JSON result after its dependencies finish.
Use **Wait for an external signal** to configure its name, label and expiry. Saving/editing does
not activate or deliver anything. Each activated run/task has an exact signal identity;
name-only broadcasts and pre-activation mailboxes are not provided. The authenticated API
commits one bounded payload and wakeup atomically; exact original delivery replay cannot
revive cancelled work. The **External signals** run card is read-only. See the
[signal operation and recovery contract](WORKFLOW_SIGNALS.md).

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

SOP v1 is available on direct workflow submission. Schedule/runtime-skill authoring may still require a tasks-based bundle; the saved library does not add schedule/runtime-skill authoring support. Pre-activation signal mailboxes, broadcast/publisher authorization, person/role-bound process approvals, general branch transitions, conditional loops, compensation and active-run version migrations are explicitly future work in [the architecture](ARCHITECTURE_CURRENT.md). The independent [work ledger](AUTONOMOUS_WORK.md) remains separate from the scheduler-level decision node.
