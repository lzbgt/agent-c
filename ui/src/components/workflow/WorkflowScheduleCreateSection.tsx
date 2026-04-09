import React from "react";

import { coerceScheduleSpec, SCHEDULE_PRESETS, SCHEDULE_SAMPLE_SPEC, validateCronExpr, validateScheduleSpec } from "./workflowPanelUtils";
import type { WorkflowSchedulesSectionProps } from "./workflowScheduleSectionTypes";

type WorkflowScheduleCreateSectionProps = Pick<
  WorkflowSchedulesSectionProps,
  | "scheduleCron"
  | "setScheduleCron"
  | "scheduleSpec"
  | "setScheduleSpec"
  | "scheduleError"
  | "setScheduleError"
  | "scheduleValidation"
  | "setScheduleValidation"
  | "scheduleCronValidation"
  | "setScheduleCronValidation"
  | "scheduleCreateBusy"
  | "onCreateSchedule"
  | "onLoadSpecFromWorkflow"
  | "onCopyText"
  | "scheduleCreateCurlSnippet"
>;

export default function WorkflowScheduleCreateSection(props: WorkflowScheduleCreateSectionProps) {
  return (
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
        <div className="rounded border border-white/10 bg-black/40 px-2 py-1 text-[11px] text-white/50">timezone UTC</div>
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
              const spec = coerceScheduleSpec(parsed);
              if (!spec) {
                props.setScheduleError("spec validation failed");
                return;
              }
              void props.onCopyText("schedule create curl", props.scheduleCreateCurlSnippet(cron, spec));
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
        placeholder='WorkflowSubmitRequest JSON (e.g. {"tasks":[{"task_id":"TASK_1","request":{"prompt":"...","no_session":true}}]})'
      />
      {props.scheduleError ? <div className="text-[11px] text-rose-200">{props.scheduleError}</div> : null}
      {props.scheduleCronValidation.length > 0 ? (
        <div className="rounded border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-[11px] text-amber-100">
          {props.scheduleCronValidation.map((message, index) => (
            <div key={`cron-${message}-${index}`}>{message}</div>
          ))}
        </div>
      ) : null}
      {props.scheduleValidation.length > 0 ? (
        <div className="rounded border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-[11px] text-amber-100">
          {props.scheduleValidation.map((message, index) => (
            <div key={`${message}-${index}`}>{message}</div>
          ))}
        </div>
      ) : null}
    </div>
  );
}
