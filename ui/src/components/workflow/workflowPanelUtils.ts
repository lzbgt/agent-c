import type {
  WorkflowDetailResp,
  WorkflowListResp,
  WorkflowScheduleListResp,
  WorkflowScheduleRunsResp,
} from "../../api";
import type {
  WorkflowBudgetSnapshot,
  WorkflowScheduleCreatePayload,
  WorkflowScheduleRow,
  WorkflowScheduleRunRow,
  WorkflowSummaryRow,
  WorkflowTaskRow,
  WorkflowSubmitRequest,
} from "../../workflowTypes";

export type WorkflowTask = WorkflowTaskRow & { depends_on: string[] };

export const STATUS_OPTIONS = ["running", "queued", "active", "done", "error", "cancelled", "all"];
export const SCHEDULE_STATUS_OPTIONS = ["active", "paused", "error", "all"];
export const SCHEDULE_RUN_STATUS_OPTIONS = ["all", "queued", "running", "done", "error"];
export const SCHEDULE_PRESETS = [
  { label: "Every 15 min", cron: "*/15 * * * *" },
  { label: "Hourly", cron: "0 * * * *" },
  { label: "Daily 09:00", cron: "0 9 * * *" },
  { label: "Weekdays 09:00", cron: "0 9 * * 1-5" },
  { label: "Weekly Mon 09:00", cron: "0 9 * * 1" },
  { label: "Monthly 1st 09:00", cron: "0 9 1 * *" },
];
export const SCHEDULE_SAMPLE_SPEC = {
  tasks: [
    {
      task_id: "TASK_1",
      request: {
        prompt: "Summarize the top 3 operational alerts from the last 24h.",
        no_session: true,
      },
    },
  ],
  defaults: {
    model: "gpt-4o-mini",
    max_steps: 6,
  },
};

const isObjectRecord = (value: unknown): value is Record<string, unknown> =>
  !!value && typeof value === "object" && !Array.isArray(value);

const toStringArray = (value: unknown): string[] =>
  Array.isArray(value) ? value.filter((item): item is string => typeof item === "string") : [];

export function normalizeTask(raw: unknown): WorkflowTask | null {
  if (!isObjectRecord(raw)) return null;
  const taskId = typeof raw.task_id === "string" ? raw.task_id : "";
  if (!taskId) return null;
  const deps = toStringArray(raw.depends_on);
  return {
    task_id: taskId,
    status: typeof raw.status === "string" ? raw.status : undefined,
    depends_on: deps,
    allow_error: typeof raw.allow_error === "boolean" ? raw.allow_error : undefined,
    attempt: typeof raw.attempt === "number" ? raw.attempt : undefined,
    max_attempts: typeof raw.max_attempts === "number" ? raw.max_attempts : undefined,
    error: typeof raw.error === "string" ? raw.error : undefined,
    ready_unix_ms: typeof raw.ready_unix_ms === "number" ? raw.ready_unix_ms : undefined,
    started_unix_ms: typeof raw.started_unix_ms === "number" ? raw.started_unix_ms : undefined,
    finished_unix_ms: typeof raw.finished_unix_ms === "number" ? raw.finished_unix_ms : undefined,
  };
}

export function normalizeWorkflowSummary(raw: unknown): WorkflowSummaryRow | null {
  if (!isObjectRecord(raw)) return null;
  const workflowId = typeof raw.workflow_id === "string" ? raw.workflow_id.trim() : "";
  if (!workflowId) return null;
  return {
    ...raw,
    workflow_id: workflowId,
    status: typeof raw.status === "string" ? raw.status : undefined,
    priority: typeof raw.priority === "number" ? raw.priority : undefined,
    deadline_unix_ms: typeof raw.deadline_unix_ms === "number" ? raw.deadline_unix_ms : undefined,
    idempotency_key: typeof raw.idempotency_key === "string" ? raw.idempotency_key : undefined,
    trace_id: typeof raw.trace_id === "string" ? raw.trace_id : undefined,
    session_id: typeof raw.session_id === "string" ? raw.session_id : undefined,
    cancel_requested: typeof raw.cancel_requested === "boolean" ? raw.cancel_requested : undefined,
    error: typeof raw.error === "string" ? raw.error : undefined,
    created_unix_ms: typeof raw.created_unix_ms === "number" ? raw.created_unix_ms : undefined,
    updated_unix_ms: typeof raw.updated_unix_ms === "number" ? raw.updated_unix_ms : undefined,
  };
}

const normalizeBudgetSnapshot = (raw: unknown): WorkflowBudgetSnapshot | undefined =>
  isObjectRecord(raw) ? (raw as WorkflowBudgetSnapshot) : undefined;

export function normalizeSchedule(raw: unknown): WorkflowScheduleRow | null {
  if (!isObjectRecord(raw)) return null;
  const scheduleId = typeof raw.schedule_id === "string" ? raw.schedule_id.trim() : "";
  if (!scheduleId) return null;
  return {
    ...raw,
    schedule_id: scheduleId,
    status: typeof raw.status === "string" ? raw.status : undefined,
    cron: typeof raw.cron === "string" ? raw.cron : undefined,
    timezone: typeof raw.timezone === "string" ? raw.timezone : undefined,
    created_unix_ms: typeof raw.created_unix_ms === "number" ? raw.created_unix_ms : undefined,
    updated_unix_ms: typeof raw.updated_unix_ms === "number" ? raw.updated_unix_ms : undefined,
    last_tick_unix_ms: typeof raw.last_tick_unix_ms === "number" ? raw.last_tick_unix_ms : undefined,
    next_tick_unix_ms: typeof raw.next_tick_unix_ms === "number" ? raw.next_tick_unix_ms : undefined,
    last_error: typeof raw.last_error === "string" ? raw.last_error : undefined,
    metadata: isObjectRecord(raw.metadata) ? (raw.metadata as WorkflowScheduleRow["metadata"]) : undefined,
  };
}

export function normalizeScheduleRun(raw: unknown): WorkflowScheduleRunRow | null {
  if (!isObjectRecord(raw)) return null;
  const scheduleId = typeof raw.schedule_id === "string" ? raw.schedule_id.trim() : "";
  const workflowId = typeof raw.workflow_id === "string" ? raw.workflow_id.trim() : "";
  if (!scheduleId || !workflowId) return null;
  return {
    ...raw,
    schedule_id: scheduleId,
    workflow_id: workflowId,
    tick_unix_ms: typeof raw.tick_unix_ms === "number" ? raw.tick_unix_ms : undefined,
    created_unix_ms: typeof raw.created_unix_ms === "number" ? raw.created_unix_ms : undefined,
    status: typeof raw.status === "string" ? raw.status : undefined,
    error: typeof raw.error === "string" ? raw.error : undefined,
  };
}

export function formatUnixMs(ms?: number): string {
  if (!ms || !Number.isFinite(ms)) return "—";
  try {
    return new Date(ms).toLocaleString();
  } catch {
    return String(ms);
  }
}

export function statusBadge(status?: string) {
  const s = String(status || "").toLowerCase();
  if (s === "done") return "bg-emerald-500/15 text-emerald-200 border-emerald-500/30";
  if (s === "running") return "bg-sky-500/15 text-sky-200 border-sky-500/30";
  if (s === "queued") return "bg-amber-500/15 text-amber-200 border-amber-500/30";
  if (s === "error") return "bg-rose-500/15 text-rose-200 border-rose-500/30";
  if (s === "cancelled") return "bg-slate-500/20 text-slate-200 border-slate-500/30";
  return "bg-white/10 text-white/70 border-white/10";
}

export function canCancelStatus(status?: string) {
  const s = String(status || "").toLowerCase();
  return s === "running" || s === "queued";
}

export function buildLevels(tasks: WorkflowTask[]) {
  const ids = new Set(tasks.map((t) => t.task_id));
  const depsMap = new Map<string, string[]>();
  const missingDeps = new Set<string>();
  for (const task of tasks) {
    const deps = task.depends_on.filter((d) => ids.has(d));
    for (const d of task.depends_on) {
      if (!ids.has(d)) missingDeps.add(d);
    }
    depsMap.set(task.task_id, deps);
  }

  const levels = new Map<string, number>();
  const visiting = new Set<string>();
  let hasCycle = false;

  const computeLevel = (id: string): number => {
    if (levels.has(id)) return levels.get(id) ?? 0;
    if (visiting.has(id)) {
      hasCycle = true;
      return 0;
    }
    visiting.add(id);
    const deps = depsMap.get(id) ?? [];
    let maxDep = -1;
    for (const dep of deps) {
      maxDep = Math.max(maxDep, computeLevel(dep));
    }
    visiting.delete(id);
    const level = maxDep + 1;
    levels.set(id, level);
    return level;
  };

  tasks.forEach((t) => computeLevel(t.task_id));

  const buckets = new Map<number, WorkflowTask[]>();
  for (const task of tasks) {
    const lvl = levels.get(task.task_id) ?? 0;
    const arr = buckets.get(lvl) ?? [];
    arr.push(task);
    buckets.set(lvl, arr);
  }
  const maxLevel = Math.max(0, ...Array.from(buckets.keys()));
  const orderedLevels = [];
  for (let i = 0; i <= maxLevel; i += 1) {
    const arr = buckets.get(i) ?? [];
    arr.sort((a, b) => a.task_id.localeCompare(b.task_id));
    orderedLevels.push(arr);
  }

  return { levels: orderedLevels, hasCycle, missingDeps: Array.from(missingDeps).sort() };
}

export function extractWorkflows(resp?: WorkflowListResp | null) {
  if (!resp || !resp.ok || !Array.isArray(resp.workflows)) return [];
  return resp.workflows
    .map((workflow) => normalizeWorkflowSummary(workflow))
    .filter((workflow): workflow is WorkflowSummaryRow => !!workflow);
}

export function extractTasks(resp?: WorkflowDetailResp | null): WorkflowTask[] {
  if (!resp || !Array.isArray(resp.tasks)) return [];
  const out: WorkflowTask[] = [];
  for (const t of resp.tasks) {
    const norm = normalizeTask(t);
    if (norm) out.push(norm);
  }
  return out;
}

export function extractWorkflowSummary(resp?: WorkflowDetailResp | null): WorkflowSummaryRow | null {
  return normalizeWorkflowSummary(resp?.workflow);
}

export function countByStatus(tasks: WorkflowTask[]) {
  const counts: Record<string, number> = {};
  for (const task of tasks) {
    const s = String(task.status || "unknown").toLowerCase();
    counts[s] = (counts[s] ?? 0) + 1;
  }
  return counts;
}

export function extractSchedules(resp?: WorkflowScheduleListResp | null): WorkflowScheduleRow[] {
  if (!resp || !Array.isArray(resp.schedules)) return [];
  return resp.schedules
    .map((schedule) => normalizeSchedule(schedule))
    .filter((schedule): schedule is WorkflowScheduleRow => !!schedule);
}

export function extractScheduleRuns(resp?: WorkflowScheduleRunsResp | null): WorkflowScheduleRunRow[] {
  if (!resp || !Array.isArray(resp.runs)) return [];
  return resp.runs
    .map((run) => normalizeScheduleRun(run))
    .filter((run): run is WorkflowScheduleRunRow => !!run);
}

export function validateCronExpr(expr: string): string[] {
  const issues: string[] = [];
  const trimmed = String(expr || "").trim();
  if (!trimmed) {
    issues.push("cron is required");
    return issues;
  }
  const parts = trimmed.split(/\s+/);
  if (parts.length !== 5) {
    issues.push("cron must have 5 fields (min hour day month weekday)");
    return issues;
  }
  const ranges: Array<[number, number, string]> = [
    [0, 59, "minute"],
    [0, 23, "hour"],
    [1, 31, "day of month"],
    [1, 12, "month"],
    [0, 7, "weekday"],
  ];
  const parseAtom = (token: string, min: number, max: number, label: string): string | null => {
    if (token === "*") return null;
    const stepMatch = token.match(/^\*\/(\d+)$/);
    if (stepMatch) {
      const step = Number(stepMatch[1]);
      if (!Number.isFinite(step) || step <= 0) return `${label} step must be > 0`;
      return null;
    }
    const rangeMatch = token.match(/^(\d+)-(\d+)$/);
    if (rangeMatch) {
      const start = Number(rangeMatch[1]);
      const end = Number(rangeMatch[2]);
      if (!Number.isFinite(start) || !Number.isFinite(end)) return `${label} range must be numeric`;
      if (start > end) return `${label} range start must be <= end`;
      if (start < min || end > max) return `${label} range must be within ${min}-${max}`;
      return null;
    }
    const single = Number(token);
    if (!Number.isFinite(single)) return `${label} entry "${token}" is invalid`;
    if (single < min || single > max) return `${label} entry must be within ${min}-${max}`;
    return null;
  };
  parts.forEach((part, idx) => {
    const [min, max, label] = ranges[idx];
    const tokens = part.split(",");
    tokens.forEach((token) => {
      const issue = parseAtom(token, min, max, label);
      if (issue) issues.push(issue);
    });
  });
  return issues;
}

export function validateScheduleSpec(spec: unknown): string[] {
  const issues: string[] = [];
  if (!isObjectRecord(spec)) {
    issues.push("spec must be a JSON object");
    return issues;
  }
  if (!Array.isArray(spec.tasks) || spec.tasks.length === 0) {
    issues.push("spec.tasks must be a non-empty array");
  } else {
    spec.tasks.forEach((task, idx) => {
      if (!isObjectRecord(task)) {
        issues.push(`task[${idx}] must be an object`);
        return;
      }
      if (!task.task_id || typeof task.task_id !== "string") {
        issues.push(`task[${idx}].task_id is required`);
      }
      if (task.depends_on !== undefined && !Array.isArray(task.depends_on)) {
        issues.push(`task[${idx}].depends_on must be an array when provided`);
      }
      const hasRequest = isObjectRecord(task.request);
      const hasKind = typeof task.kind === "string" && task.kind.trim().length > 0;
      if (!hasRequest && !hasKind) {
        issues.push(`task[${idx}] must define request or kind`);
      }
    });
  }
  if (spec.defaults !== undefined && !isObjectRecord(spec.defaults)) {
    issues.push("spec.defaults must be an object when provided");
  }
  return issues;
}

export function coerceScheduleSpec(spec: unknown): WorkflowScheduleCreatePayload["spec"] | null {
  return validateScheduleSpec(spec).length === 0 ? (spec as WorkflowSubmitRequest) : null;
}

export function extractWorkflowLimits(resp?: WorkflowDetailResp | null): WorkflowBudgetSnapshot | undefined {
  return normalizeBudgetSnapshot(resp?.workflow_limits);
}

export function extractWorkflowUsage(resp?: WorkflowDetailResp | null): WorkflowBudgetSnapshot | undefined {
  return normalizeBudgetSnapshot(resp?.workflow_usage);
}

export function extractWorkflowRemaining(resp?: WorkflowDetailResp | null): WorkflowBudgetSnapshot | undefined {
  return normalizeBudgetSnapshot(resp?.workflow_remaining);
}
