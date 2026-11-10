import React from "react";
import FieldLabel from "../FieldLabel";
import type { MemoryRecapsState } from "./useMemoryPanelState";

type Props = { canQuery: boolean; stringifyJson: (value: unknown) => string; recaps: MemoryRecapsState };

export default function MemoryRecapScheduleSection(props: Props) {
  return (
      <div className="rounded-md border border-white/10 bg-black/30 p-3">
        <div className="flex items-center justify-between gap-2">
          <div className="text-[11px] font-semibold text-white/80">Recap scheduling</div>
          <div className="flex items-center gap-2">
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={props.recaps.schedule.busy || !props.canQuery}
              onClick={() => void props.recaps.schedule.load()}
            >
              {props.recaps.schedule.busy ? "Loading…" : "Load config"}
            </button>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={props.recaps.schedule.busy || !props.canQuery}
              onClick={() => void props.recaps.schedule.apply()}
            >
              {props.recaps.schedule.busy ? "Saving…" : "Apply schedule"}
            </button>
          </div>
        </div>
        <div className="mt-2 grid gap-2 text-[11px] text-white/70">
          <div className="grid grid-cols-2 gap-3">
            <div>
              <FieldLabel>Daily interval (ms)</FieldLabel>
              <input
                data-testid="memory-recap-daily-interval-input"
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                value={props.recaps.schedule.dailyIntervalMs}
                onChange={(e) => props.recaps.schedule.setDailyIntervalMs(e.target.value)}
                inputMode="numeric"
              />
            </div>
            <div>
              <FieldLabel>Weekly interval (ms)</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                value={props.recaps.schedule.weeklyIntervalMs}
                onChange={(e) => props.recaps.schedule.setWeeklyIntervalMs(e.target.value)}
                inputMode="numeric"
              />
            </div>
          </div>
          <div className="grid grid-cols-2 gap-3">
            <div>
              <FieldLabel>Daily days</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                value={props.recaps.schedule.dailyDays}
                onChange={(e) => props.recaps.schedule.setDailyDays(e.target.value)}
                inputMode="numeric"
              />
            </div>
            <div>
              <FieldLabel>Weekly days</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                value={props.recaps.schedule.weeklyDays}
                onChange={(e) => props.recaps.schedule.setWeeklyDays(e.target.value)}
                inputMode="numeric"
              />
            </div>
          </div>
          <div className="text-[11px] text-white/50">
            Summary model: {props.recaps.schedule.summaryModel ? props.recaps.schedule.summaryModel : "unset"} (scheduled recaps require summary_model)
          </div>
        </div>
        {props.recaps.schedule.error ? (
          <div className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
            {props.recaps.schedule.error}
          </div>
        ) : null}
        {props.recaps.schedule.result ? (
          <pre className="mt-2 max-h-56 overflow-auto rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/70">
            {props.stringifyJson(props.recaps.schedule.result)}
          </pre>
        ) : null}
      </div>

  );
}
