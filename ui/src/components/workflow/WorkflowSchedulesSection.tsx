import React from "react";
import {
  formatUnixMs,
  SCHEDULE_PRESETS,
  SCHEDULE_RUN_STATUS_OPTIONS,
  SCHEDULE_SAMPLE_SPEC,
  SCHEDULE_STATUS_OPTIONS,
  statusBadge,
  validateCronExpr,
  validateScheduleSpec,
} from "./workflowPanelUtils";

type WorkflowSchedulesSectionProps = {
  baseUrl: string;
  normalizedScheduleStatus: string;
  scheduleStatus: string;
  setScheduleStatus: (value: string) => void;
  scheduleLimit: string;
  setScheduleLimit: (value: string) => void;
  scheduleOffset: string;
  setScheduleOffset: (value: string) => void;
  scheduleFilter: string;
  setScheduleFilter: (value: string) => void;
  scheduleAutoRefresh: boolean;
  setScheduleAutoRefresh: (value: boolean) => void;
  scheduleLimitValue: number;
  scheduleOffsetValue: number;
  scheduleRunsLimit: string;
  setScheduleRunsLimit: (value: string) => void;
  scheduleRunsOffset: string;
  setScheduleRunsOffset: (value: string) => void;
  scheduleRunsStatus: string;
  setScheduleRunsStatus: (value: string) => void;
  scheduleRunsErrorsOnly: boolean;
  setScheduleRunsErrorsOnly: (value: boolean) => void;
  scheduleRunsFilter: string;
  setScheduleRunsFilter: (value: string) => void;
  normalizedScheduleRunsStatus: string;
  scheduleRunsLimitValue: number;
  scheduleRunsOffsetValue: number;
  scheduleCron: string;
  setScheduleCron: (value: string) => void;
  scheduleSpec: string;
  setScheduleSpec: (value: string) => void;
  scheduleId: string;
  scheduleError: string | null;
  setScheduleError: (value: string | null) => void;
  scheduleValidation: string[];
  setScheduleValidation: (value: string[]) => void;
  scheduleCronValidation: string[];
  setScheduleCronValidation: (value: string[]) => void;
  scheduleBusyId: string | null;
  scheduleCreateBusy: boolean;
  scheduleListQuery: any;
  scheduleRunsQuery: any;
  scheduleList: any[];
  scheduleRuns: any[];
  filteredScheduleList: any[];
  filteredScheduleRuns: any[];
  onCopyText: (label: string, value?: string | null) => Promise<void> | void;
  onCopyJson: (label: string, payload: any) => Promise<void> | void;
  onDownloadJson: (label: string, payload: any) => void;
  onLoadScheduleRuns: (id: string) => void;
  onLoadWorkflowFromRun: (id: string) => void;
  onCreateSchedule: () => Promise<void> | void;
  onPauseSchedule: (id: string) => Promise<void> | void;
  onResumeSchedule: (id: string) => Promise<void> | void;
  onDeleteSchedule: (id: string) => Promise<void> | void;
  onLoadSpecFromWorkflow: () => void;
  scheduleCurlSnippet: (id: string, action: "pause" | "resume" | "delete") => string;
  scheduleCreateCurlSnippet: (cron: string, spec: any) => string;
};

export default function WorkflowSchedulesSection(props: WorkflowSchedulesSectionProps) {
  return (
    <div className="rounded-md border border-white/10 bg-black/30 p-3" data-testid="workflow-schedules-panel">
      <div className="mb-2 flex flex-wrap items-center justify-between gap-2">
        <div className="text-xs font-semibold text-white/70">Workflow schedules (UTC)</div>
        <div className="flex flex-wrap items-center gap-2 text-[11px] text-white/60">
          <label className="flex items-center gap-1">
            status
            <select
              className="rounded border border-white/10 bg-black/40 px-1 py-0.5 text-[11px] text-white/80"
              value={props.normalizedScheduleStatus}
              onChange={(e) => props.setScheduleStatus(e.target.value)}
            >
              {SCHEDULE_STATUS_OPTIONS.map((s) => (
                <option key={s} value={s}>
                  {s}
                </option>
              ))}
            </select>
          </label>
          <label className="flex items-center gap-1">
            limit
            <input
              className="w-[56px] rounded border border-white/10 bg-black/40 px-1 py-0.5 text-[11px] text-white/80"
              value={String(props.scheduleLimit || "")}
              onChange={(e) => props.setScheduleLimit(e.target.value)}
            />
          </label>
          <label className="flex items-center gap-1">
            offset
            <input
              className="w-[56px] rounded border border-white/10 bg-black/40 px-1 py-0.5 text-[11px] text-white/80"
              value={String(props.scheduleOffset || "")}
              onChange={(e) => props.setScheduleOffset(e.target.value)}
            />
          </label>
          <label className="flex items-center gap-1">
            filter
            <span className="flex items-center gap-1">
              <input
                className="w-[140px] rounded border border-white/10 bg-black/40 px-1 py-0.5 text-[11px] text-white/80"
                value={String(props.scheduleFilter || "")}
                onChange={(e) => props.setScheduleFilter(e.target.value)}
                placeholder="id/cron/error"
              />
              {String(props.scheduleFilter || "").trim() ? (
                <button
                  className="rounded border border-white/10 px-1 py-0.5 text-[10px] text-white/60 hover:bg-white/5"
                  type="button"
                  onClick={() => props.setScheduleFilter("")}
                >
                  clear
                </button>
              ) : null}
            </span>
          </label>
          <label className="flex items-center gap-1">
            <input
              type="checkbox"
              className="h-3 w-3"
              checked={!!props.scheduleAutoRefresh}
              onChange={(e) => props.setScheduleAutoRefresh(e.target.checked)}
            />
            auto
          </label>
          <div className="flex items-center gap-1">
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
              type="button"
              onClick={() => props.setScheduleOffset(String(Math.max(0, props.scheduleOffsetValue - props.scheduleLimitValue)))}
              disabled={props.scheduleOffsetValue === 0}
            >
              Prev
            </button>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
              type="button"
              onClick={() => props.setScheduleOffset(String(props.scheduleOffsetValue + props.scheduleLimitValue))}
              disabled={props.scheduleList.length < props.scheduleLimitValue}
            >
              Next
            </button>
          </div>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
            type="button"
            onClick={() => props.scheduleListQuery.refetch()}
            disabled={props.scheduleListQuery.isFetching}
          >
            {props.scheduleListQuery.isFetching ? "Refreshing…" : "Refresh"}
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
            type="button"
            onClick={() => void props.onCopyJson("schedules", props.filteredScheduleList)}
            disabled={props.filteredScheduleList.length === 0}
          >
            Copy JSON
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
            type="button"
            onClick={() => props.onDownloadJson("workflow-schedules", props.filteredScheduleList)}
            disabled={props.filteredScheduleList.length === 0}
          >
            Download JSON
          </button>
        </div>
      </div>

      <div className="grid gap-2">
        <div className="grid gap-2 rounded-md border border-white/10 bg-black/40 p-2">
          <div className="text-[11px] text-white/60">Create schedule</div>
          <div className="flex flex-wrap items-center gap-2 text-[10px] text-white/60">
            <span>Presets</span>
            {SCHEDULE_PRESETS.map((preset) => (
              <button
                key={preset.label}
                className="rounded border border-white/10 px-2 py-1 text-[10px] text-white/70 hover:bg-white/5"
                type="button"
                onClick={() => props.setScheduleCron(preset.cron)}
              >
                {preset.label}
              </button>
            ))}
          </div>
          <div className="flex flex-wrap items-center gap-2">
            <input
              data-testid="workflow-schedule-cron"
              className="min-w-[160px] flex-1 rounded border border-white/10 bg-black/40 px-2 py-1 text-[11px] text-white/80"
              value={String(props.scheduleCron || "")}
              onChange={(e) => {
                const next = e.target.value;
                props.setScheduleCron(next);
                if (props.scheduleError) props.setScheduleError(null);
                props.setScheduleCronValidation(validateCronExpr(next));
              }}
              placeholder="cron (e.g. 0 9 * * 1-5)"
            />
            <div className="rounded border border-white/10 bg-black/40 px-2 py-1 text-[11px] text-white/50">
              timezone UTC
            </div>
            <button
              className="rounded border border-white/10 bg-black/40 px-2 py-1 text-[11px] text-white/70 hover:bg-black/50"
              type="button"
              onClick={() => props.onLoadSpecFromWorkflow()}
            >
              use loaded spec
            </button>
            <button
              className="rounded border border-white/10 bg-black/40 px-2 py-1 text-[11px] text-white/70 hover:bg-black/50"
              type="button"
              onClick={() => {
                const payload = JSON.stringify(SCHEDULE_SAMPLE_SPEC, null, 2);
                props.setScheduleSpec(payload);
                props.setScheduleValidation(validateScheduleSpec(SCHEDULE_SAMPLE_SPEC));
              }}
            >
              insert sample spec
            </button>
            <button
              data-testid="workflow-schedule-create"
              className="rounded border border-white/10 bg-black/40 px-2 py-1 text-[11px] text-white/80 hover:bg-black/50 disabled:opacity-50"
              type="button"
              onClick={() => void props.onCreateSchedule()}
              disabled={props.scheduleCreateBusy}
            >
              {props.scheduleCreateBusy ? "Creating…" : "Create"}
            </button>
            <button
              className="rounded border border-white/10 bg-black/40 px-2 py-1 text-[11px] text-white/80 hover:bg-black/50"
              type="button"
              onClick={() => {
                const cron = String(props.scheduleCron || "").trim();
                const cronIssues = validateCronExpr(cron);
                if (cronIssues.length > 0) {
                  props.setScheduleCronValidation(cronIssues);
                  props.setScheduleError("cron validation failed");
                  return;
                }
                const specRaw = String(props.scheduleSpec || "").trim();
                if (!specRaw) {
                  props.setScheduleError("spec JSON is required");
                  return;
                }
                try {
                  const parsed = JSON.parse(specRaw);
                  const issues = validateScheduleSpec(parsed);
                  props.setScheduleValidation(issues);
                  if (issues.length > 0) {
                    props.setScheduleError("spec validation failed");
                    return;
                  }
                  void props.onCopyText("schedule create curl", props.scheduleCreateCurlSnippet(cron, parsed));
                } catch (err) {
                  props.setScheduleError(`spec JSON parse error: ${String(err)}`);
                }
              }}
            >
              copy create curl
            </button>
          </div>
          <textarea
            data-testid="workflow-schedule-spec"
            className="min-h-[120px] rounded border border-white/10 bg-black/40 px-2 py-2 text-[11px] text-white/80"
            value={String(props.scheduleSpec || "")}
            onChange={(e) => {
              props.setScheduleSpec(e.target.value);
              if (props.scheduleError) props.setScheduleError(null);
            }}
            placeholder='spec JSON (workflow submit payload, e.g. {"tasks":[...], "defaults":{...}})'
          />
          {props.scheduleError ? <div className="text-[11px] text-rose-200">{props.scheduleError}</div> : null}
          {props.scheduleCronValidation.length > 0 ? (
            <div className="rounded border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-[11px] text-amber-100">
              {props.scheduleCronValidation.map((msg, idx) => (
                <div key={`cron-${msg}-${idx}`}>{msg}</div>
              ))}
            </div>
          ) : null}
          {props.scheduleValidation.length > 0 ? (
            <div className="rounded border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-[11px] text-amber-100">
              {props.scheduleValidation.map((msg, idx) => (
                <div key={`${msg}-${idx}`}>{msg}</div>
              ))}
            </div>
          ) : null}
        </div>

        {!props.baseUrl ? (
          <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-xs text-amber-100">
            Set a daemon base URL to manage schedules.
          </div>
        ) : null}
        {props.scheduleListQuery.isError ? (
          <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-xs text-rose-200">
            {String(props.scheduleListQuery.error)}
          </div>
        ) : null}

        <div className="grid gap-2">
          {props.filteredScheduleList.map((sched: any) => {
            const id = String(sched.schedule_id || "").trim();
            const status = String(sched.status || "").toLowerCase();
            return (
              <div
                data-testid={`workflow-schedule-row-${id}`}
                key={id || Math.random()}
                className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-white/10 bg-black/40 px-2 py-2 text-left text-xs text-white/80"
              >
                <button
                  type="button"
                  onClick={() => {
                    if (!id) return;
                    props.onLoadScheduleRuns(id);
                  }}
                  className="flex flex-1 flex-wrap items-center justify-between gap-2 text-left hover:text-white"
                >
                  <div className="flex flex-wrap items-center gap-2">
                    <span className={`rounded border px-2 py-0.5 text-[10px] ${statusBadge(status)}`}>
                      {status || "unknown"}
                    </span>
                    <span className="font-mono text-[11px] text-white/80">{id}</span>
                    {sched.cron ? <span className="text-[10px] text-white/50">cron {String(sched.cron)}</span> : null}
                    {sched.next_tick_unix_ms ? (
                      <span className="text-[10px] text-white/50">next {formatUnixMs(sched.next_tick_unix_ms)}</span>
                    ) : null}
                    {sched.last_error ? <span className="text-[10px] text-rose-200">err {String(sched.last_error)}</span> : null}
                  </div>
                  <div className="text-[10px] text-white/40">updated {formatUnixMs(sched.updated_unix_ms)}</div>
                </button>
                <div className="flex items-center gap-2">
                  {id ? (
                    <button
                      className="rounded border border-white/10 px-2 py-1 text-[10px] text-white/60 hover:bg-white/5"
                      type="button"
                      onClick={() => void props.onCopyText("schedule id", id)}
                    >
                      copy id
                    </button>
                  ) : null}
                  {id ? (
                    <button
                      className="rounded border border-white/10 px-2 py-1 text-[10px] text-white/60 hover:bg-white/5"
                      type="button"
                      onClick={() => void props.onCopyText("schedule pause curl", props.scheduleCurlSnippet(id, "pause"))}
                    >
                      copy pause curl
                    </button>
                  ) : null}
                  {id ? (
                    <button
                      className="rounded border border-white/10 px-2 py-1 text-[10px] text-white/60 hover:bg-white/5"
                      type="button"
                      onClick={() => void props.onCopyText("schedule resume curl", props.scheduleCurlSnippet(id, "resume"))}
                    >
                      copy resume curl
                    </button>
                  ) : null}
                  {id ? (
                    <button
                      className="rounded border border-white/10 px-2 py-1 text-[10px] text-white/60 hover:bg-white/5"
                      type="button"
                      onClick={() => void props.onCopyText("schedule delete curl", props.scheduleCurlSnippet(id, "delete"))}
                    >
                      copy delete curl
                    </button>
                  ) : null}
                  {status === "active" ? (
                    <button
                      className="rounded border border-white/10 px-2 py-1 text-[10px] text-white/60 hover:bg-white/5 disabled:opacity-50"
                      type="button"
                      onClick={() => void props.onPauseSchedule(id)}
                      disabled={props.scheduleBusyId === id}
                    >
                      pause
                    </button>
                  ) : (
                    <button
                      className="rounded border border-white/10 px-2 py-1 text-[10px] text-white/60 hover:bg-white/5 disabled:opacity-50"
                      type="button"
                      onClick={() => void props.onResumeSchedule(id)}
                      disabled={props.scheduleBusyId === id}
                    >
                      resume
                    </button>
                  )}
                  <button
                    className="rounded border border-rose-500/30 px-2 py-1 text-[10px] text-rose-200 hover:bg-rose-500/10 disabled:opacity-50"
                    type="button"
                    onClick={() => void props.onDeleteSchedule(id)}
                    disabled={props.scheduleBusyId === id}
                  >
                    delete
                  </button>
                </div>
              </div>
            );
          })}
          {props.scheduleListQuery.isSuccess && props.filteredScheduleList.length === 0 ? (
            <div className="rounded border border-white/10 bg-black/20 px-2 py-2 text-[11px] text-white/50">
              {props.scheduleList.length === 0 ? "No schedules yet." : "No schedules match the filter."}
            </div>
          ) : null}
        </div>

        <div className="grid gap-2 rounded-md border border-white/10 bg-black/40 p-2" data-testid="workflow-schedule-runs-panel">
          <div className="flex flex-wrap items-center justify-between gap-2">
            <div className="text-[11px] text-white/60">Schedule runs</div>
            <div className="flex items-center gap-2 text-[11px] text-white/60">
              <label className="flex items-center gap-1">
                limit
                <input
                  className="w-[56px] rounded border border-white/10 bg-black/40 px-1 py-0.5 text-[11px] text-white/80"
                  value={String(props.scheduleRunsLimit || "")}
                  onChange={(e) => props.setScheduleRunsLimit(e.target.value)}
                />
              </label>
              <label className="flex items-center gap-1">
                offset
                <input
                  className="w-[56px] rounded border border-white/10 bg-black/40 px-1 py-0.5 text-[11px] text-white/80"
                  value={String(props.scheduleRunsOffset || "")}
                  onChange={(e) => props.setScheduleRunsOffset(e.target.value)}
                />
              </label>
              <label className="flex items-center gap-1">
                status
                <select
                  className="rounded border border-white/10 bg-black/40 px-1 py-0.5 text-[11px] text-white/80"
                  value={props.normalizedScheduleRunsStatus}
                  onChange={(e) => props.setScheduleRunsStatus(e.target.value)}
                >
                  {SCHEDULE_RUN_STATUS_OPTIONS.map((s) => (
                    <option key={s} value={s}>
                      {s}
                    </option>
                  ))}
                </select>
              </label>
              <label className="flex items-center gap-1">
                <input
                  type="checkbox"
                  className="h-3 w-3"
                  checked={!!props.scheduleRunsErrorsOnly}
                  onChange={(e) => props.setScheduleRunsErrorsOnly(e.target.checked)}
                />
                errors only
              </label>
              <label className="flex items-center gap-1">
                filter
                <span className="flex items-center gap-1">
                  <input
                    className="w-[120px] rounded border border-white/10 bg-black/40 px-1 py-0.5 text-[11px] text-white/80"
                    value={String(props.scheduleRunsFilter || "")}
                    onChange={(e) => props.setScheduleRunsFilter(e.target.value)}
                    placeholder="workflow id"
                  />
                  {String(props.scheduleRunsFilter || "").trim() ? (
                    <button
                      className="rounded border border-white/10 px-1 py-0.5 text-[10px] text-white/60 hover:bg-white/5"
                      type="button"
                      onClick={() => props.setScheduleRunsFilter("")}
                    >
                      clear
                    </button>
                  ) : null}
                </span>
              </label>
              <div className="flex items-center gap-1">
                <button
                  className="rounded border border-white/10 px-2 py-1 text-[10px] text-white/60 hover:bg-white/5 disabled:opacity-50"
                  type="button"
                  onClick={() =>
                    props.setScheduleRunsOffset(String(Math.max(0, props.scheduleRunsOffsetValue - props.scheduleRunsLimitValue)))
                  }
                  disabled={props.scheduleRunsOffsetValue === 0}
                >
                  Prev
                </button>
                <button
                  className="rounded border border-white/10 px-2 py-1 text-[10px] text-white/60 hover:bg-white/5 disabled:opacity-50"
                  type="button"
                  onClick={() => props.setScheduleRunsOffset(String(props.scheduleRunsOffsetValue + props.scheduleRunsLimitValue))}
                  disabled={props.scheduleRuns.length < props.scheduleRunsLimitValue}
                >
                  Next
                </button>
              </div>
              <button
                className="rounded border border-white/10 px-2 py-1 text-[10px] text-white/60 hover:bg-white/5"
                type="button"
                onClick={() => props.scheduleRunsQuery.refetch()}
                disabled={props.scheduleRunsQuery.isFetching || !String(props.scheduleId || "").trim()}
              >
                {props.scheduleRunsQuery.isFetching ? "Refreshing…" : "Refresh"}
              </button>
              <button
                className="rounded border border-white/10 px-2 py-1 text-[10px] text-white/60 hover:bg-white/5 disabled:opacity-50"
                type="button"
                onClick={() => void props.onCopyJson("schedule runs", props.scheduleRuns)}
                disabled={props.scheduleRuns.length === 0}
              >
                Copy JSON
              </button>
              <button
                className="rounded border border-white/10 px-2 py-1 text-[10px] text-white/60 hover:bg-white/5 disabled:opacity-50"
                type="button"
                onClick={() => props.onDownloadJson("workflow-schedule-runs", props.scheduleRuns)}
                disabled={props.scheduleRuns.length === 0}
              >
                Download JSON
              </button>
            </div>
          </div>
          <div className="text-[10px] text-white/50">schedule_id: {props.scheduleId ? props.scheduleId : "—"}</div>
          {props.scheduleRunsQuery.isError ? (
            <div className="text-[11px] text-rose-200">{String(props.scheduleRunsQuery.error)}</div>
          ) : null}
          <div className="grid gap-2">
            {props.filteredScheduleRuns.map((run: any) => (
              <div
                key={`${run.schedule_id}-${run.tick_unix_ms}-${run.workflow_id}`}
                className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-white/10 bg-black/50 px-2 py-2 text-[11px] text-white/70"
              >
                <div className="flex flex-wrap items-center gap-2">
                  <span className={`rounded border px-2 py-0.5 text-[10px] ${statusBadge(run.status)}`}>
                    {String(run.status || "unknown")}
                  </span>
                  <span className="font-mono">{String(run.workflow_id || "")}</span>
                  <span className="text-white/50">tick {formatUnixMs(run.tick_unix_ms)}</span>
                </div>
                <div className="flex items-center gap-2">
                  {run.workflow_id ? (
                    <button
                      className="rounded border border-white/10 px-2 py-1 text-[10px] text-white/60 hover:bg-white/5"
                      type="button"
                      onClick={() => {
                        const id = String(run.workflow_id || "").trim();
                        if (!id) return;
                        props.onLoadWorkflowFromRun(id);
                      }}
                    >
                      load workflow
                    </button>
                  ) : null}
                </div>
                {run.error ? <div className="text-rose-200">{String(run.error)}</div> : null}
              </div>
            ))}
            {props.scheduleRunsQuery.isSuccess && props.filteredScheduleRuns.length === 0 ? (
              <div className="rounded border border-white/10 bg-black/20 px-2 py-2 text-[11px] text-white/50">
                {props.scheduleRuns.length === 0 ? "No runs yet." : "No runs match the filter."}
              </div>
            ) : null}
          </div>
        </div>
      </div>
    </div>
  );
}
