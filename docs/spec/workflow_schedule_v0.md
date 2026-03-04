# Workflow Schedule v0 (cron-bound workflows)

Date: 2026-03-04  
Status: draft (rolling)

## Goals

1) **First-class cron schedules**
   - Allow workflows to be submitted with a cron schedule and timezone.
   - Persist schedules durably and resume after restarts.

2) **Deterministic execution**
   - Each scheduled tick produces a workflow run with a stable
     schedule/tick identity for auditing and replay.

3) **Operator-safe controls**
   - Enable/disable schedules without deleting history.
   - Clear visibility into next/last run and failures.

## Non-goals (v0)

- Full calendar/event DSL beyond cron + timezone.
- Multi-tenant quota enforcement beyond existing workflow limits.
- UI-based cron builder (text field only in v0).

## Constraints (facts)

- The DB currently stores **one-shot** workflows in `workflows`.
- There is no persisted cron/schedule table today.
- Workflow scheduler already runs in the daemon (durable queues + fairness).
- WebUI has a Workflows panel but no schedule UI.
- Timezone support is **UTC-only** in v0.

## Proposed model

### New DB tables

**`workflow_schedules`**

```
schedule_id TEXT PRIMARY KEY
created_unix_ms INTEGER NOT NULL
updated_unix_ms INTEGER NOT NULL
status TEXT NOT NULL             -- active|paused|error
cron TEXT NOT NULL               -- 5-field cron
timezone TEXT NOT NULL           -- IANA TZ (e.g. "UTC", "America/Los_Angeles")
spec_json TEXT NOT NULL          -- workflow spec template
last_tick_unix_ms INTEGER        -- last scheduled tick time (UTC)
next_tick_unix_ms INTEGER        -- next scheduled tick time (UTC)
last_error TEXT                  -- most recent schedule error
metadata_json TEXT               -- optional: labels, owner, description
```

**`workflow_schedule_runs`** (audit / idempotency)

```
schedule_id TEXT NOT NULL
tick_unix_ms INTEGER NOT NULL    -- canonical tick time (UTC)
workflow_id TEXT NOT NULL        -- instantiated workflow id
created_unix_ms INTEGER NOT NULL
status TEXT NOT NULL             -- enqueued|running|done|error
error TEXT
PRIMARY KEY(schedule_id, tick_unix_ms)
FOREIGN KEY(schedule_id) REFERENCES workflow_schedules(schedule_id) ON DELETE CASCADE
```

### API additions (agentd)

**Create schedule**
```
POST /api/v1/workflow_schedules
{
  "cron": "0 9 * * 1-5",
  "timezone": "America/Los_Angeles",
  "spec": { ...workflow spec template... },
  "metadata": { "label": "weekday-briefing" }
}
```

**List schedules**
```
GET /api/v1/workflow_schedules
```

**Get schedule**
```
GET /api/v1/workflow_schedule?schedule_id=...
```

**Pause / resume**
```
POST /api/v1/workflow_schedule/pause
POST /api/v1/workflow_schedule/resume
```

**Delete**
```
DELETE /api/v1/workflow_schedule?schedule_id=...
```

**Runs**
```
GET /api/v1/workflow_schedule/runs?schedule_id=...
```

### Scheduler loop changes

1) **Tick scan**
   - Poll schedules with `next_tick_unix_ms <= now`.
   - For each tick, compute canonical tick time and ensure an entry in
     `workflow_schedule_runs` using the `(schedule_id, tick_unix_ms)` PK.
2) **Workflow instantiation**
   - Materialize a workflow spec from the schedule’s `spec_json`.
   - Generate a deterministic `workflow_id` with schedule + tick salt.
3) **Update next tick**
   - Compute the next tick using cron + timezone and persist.
4) **Error handling**
   - If cron parsing fails, set schedule status `error` and record `last_error`.

### Idempotency

Use `(schedule_id, tick_unix_ms)` as the **single source of truth** to avoid
double scheduling across restarts.

## Security + policy

- Schedules run under the same policy and approvals as workflows.
- If global policy forbids unattended execution, schedules should be rejected
  at creation time (policy hook).

## WebUI surface (v0)

- Workflows panel: add a “Schedules” subpanel with list + create modal.
- Show `cron`, `timezone`, `status`, `next_tick`.
- Allow pause/resume/delete.

## Migration plan

1) Add new tables + indexes (schema version bump).
2) Add schedule endpoints in agentd + OpenAPI.
3) Wire scheduler tick loop into existing workflow scheduler loop.
4) Add UI list + create form (text cron).
5) Add tests:
   - schedule creation + pause/resume
   - tick instantiation idempotency (restart safe)
   - invalid cron -> status `error`
