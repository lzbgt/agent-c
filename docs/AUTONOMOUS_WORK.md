# Autonomous requirements and human-action blockers

The default host prompt executes authorized work without permission prompts. `work_state` is the shared host tool for CLI and daemon agents (SQLite-enabled builds). `agent work` exposes the same implementation to scripts and humans without making a model call.

The ledger is separate from the daemon's main DB and from conversation history. CLI host tools use `<working-directory>/.agent/work.sqlite`. Daemon host tools use `<session-root>/work/work.sqlite`; `session-root` follows the existing host tool session layout. `agent work --file <path>` can inspect or update either file when the caller has filesystem access. Include these files in backup/erase procedures; never put credentials in evidence or blocker text.

## Commands

```sh
./build/agent work '{"op":"requirement","id":"deploy","title":"Deploy the service"}'
./build/agent work '{"op":"requirement","id":"docs","title":"Update operational documentation"}'
./build/agent work '{"op":"requirement","id":"verify","title":"Verify the deployment","depends_on":["deploy"]}'
./build/agent work '{"op":"raise_blocker","id":"access","reason":"Deployment account has no role","required_action":"Grant the deployment role","attempts":["Existing credential returned access denied","Verified no authorized alternate identity is available"],"requirement_ids":["deploy"]}'
./build/agent work
```

The result contains `state`, `runnable`, `waiting`, `open_blockers`, `all_requirements_done`, and `can_continue`. In the example, `docs` stays runnable while `deploy` and its dependent `verify` wait. A blocker with `requirement_ids: []` is advisory and blocks no requirement.

After observing a fix, verify it and resolve every affected blocker:

```sh
./build/agent work '{"op":"resolve_blocker","id":"access","evidence":"Authenticated deployment capability check now succeeds"}'
./build/agent work '{"op":"requirement","id":"deploy","status":"done","evidence":"Deployment finished and service health check passed"}'
```

`verify` then becomes runnable. Completing an upstream requirement does not complete its dependents.

## Contract

Operations are `read` (default), `requirement`, `raise_blocker`, and `resolve_blocker`. Requirement mutations upsert a single item, preserving omitted fields. New requirements need `id` and `title`. Status is `pending`, `running`, or `done`; `depends_on` defaults to an empty array. Dependencies must already exist and form a DAG. Done requires a nonempty `evidence` string and cannot bypass an unfinished dependency or open blocker. A blocker cannot be added to already completed work until that completion is explicitly reopened consistently with dependents.

Raising a blocker requires a stable ID, reason, exact required human action, 1–16 observed recovery attempts, and affected requirement IDs. Reusing the same blocker ID updates/reopens that blocker. Resolving requires a known blocker and verification evidence. These fields document the agent's claims; the runtime does not independently prove that every possible recovery was attempted or that the supplied evidence is true.

Mutations are serialized with SQLite `BEGIN IMMEDIATE` and a three-second busy timeout. Every accepted mutation increments a revision. Optional `expected_revision` rejects stale mutations; read current state and reconsider before retrying. Errors roll back. A missing ledger read returns empty state without creating files. Bounds: 256 requirements, 128 blockers, 64 KiB requests and 1 MiB persisted state. Text fields are capped, unknown operation fields rejected, and read-only host policy denies mutations. This is a current-state store, not an append-only audit journal.

## Continuation

The host executor adopts the ledger after a successful `work_state` tool call in that run. When the model proposes a final answer, a C completion hook consults the adopted ledger. If runnable requirements remain, execution continues within the original run and step/tool/cancellation budgets. When only human-dependent work remains, the run may yield with state intact. An old ledger is not automatically adopted by an unrelated prompt.

Three further proposed completions at an unchanged ledger revision produce a limit error. This prevents a model that repeatedly claims completion from consuming an unlimited budget. It does not invent a human blocker for a model failure.

The tool does not send a message to a human service or automatically wake a stopped process. CLI callers resume explicitly; daemon/broker event-driven wakeup and a dedicated blocker API/UI are design backlog. Operator stops remain authoritative. For embedded applications, `agent_tool_loop_hooks_t.on_proposed_completion` can use another bounded state implementation; it does not require SQLite.
