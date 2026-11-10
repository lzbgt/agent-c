# SOP workflows v1

`POST /api/v1/workflow/submit` accepts exactly one of the existing `tasks` array or a `sop` object. The server compiles SOP stages into the existing durable DAG before ordinary task validation, policy enforcement, admission control, persistence and scheduling. The WebUI workflow composer includes **SOP · stages and actions** as a JSON template.

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

Actions use the existing task payload contracts. Supported kinds are `llm` (or an omitted kind with `request`), `delay`, `http_json`, `agentd_call`, `aggregate`, `avm_capsule`, memory task kinds, `experience_record`, `edge_invoke`, `edge_wait_sensor`, and sequential `delegate`. Native HTTP/device/capsule policies remain effective; SOP input does not grant capabilities. See [WORKFLOWS.md](WORKFLOWS.md) and the generated OpenAPI contract for their payloads.

The stored diagnostic spec contains the original `sop`, compiled `tasks`, and server-owned `sop_compiled` metadata: compiler schema, definition ID/version, canonical definition SHA-256 and stage-to-task mapping. The hash uses `agent_json_c14n_v1`, not an unspecified JSON serialization. It identifies the submitted definition, not externally supplied defaults or secrets; the entire redacted run spec records those execution settings separately. Reusing an ID/version with changed source currently produces a different hash; immutable registry enforcement is future work.

Submission idempotency retains the existing first-accepted-wins semantics. It does not guarantee exactly-once external effects. New persisted specs above 256 KiB are rejected instead of being truncated into invalid JSON. For a fresh SOP submission, submit its `sop` and desired workflow-level settings, omitting server metadata and generated tasks. For a deliberate DAG copy, submit just its compiled tasks/settings. Diagnostic `spec` is not itself a ready-to-resubmit request.

SOP v1 is available on direct workflow submission. Schedule/runtime-skill authoring may still require a tasks-based bundle; no additional route is advertised here. Durable human signal actions, process approvals, branches, loops, compensation and registry migrations are explicitly future work in [the architecture](ARCHITECTURE_CURRENT.md). The independent [work ledger](AUTONOMOUS_WORK.md) is not a scheduler-level human-wait node.
