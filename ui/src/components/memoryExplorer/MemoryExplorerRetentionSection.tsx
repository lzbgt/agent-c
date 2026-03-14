import React from "react";
import FieldLabel from "../FieldLabel";
import type { MemoryRetentionState } from "./useMemoryPanelState";

type MemoryExplorerRetentionSectionProps = {
  canQuery: boolean;
  stringifyJson: (value: unknown) => string;
  retention: MemoryRetentionState;
};

export default function MemoryExplorerRetentionSection(props: MemoryExplorerRetentionSectionProps) {
  return (
    <section className="rounded-md border border-white/10 bg-black/20 p-3" data-testid="memory-retention-section">
      <div className="mb-2 flex items-center justify-between gap-2">
        <div className="text-xs font-semibold text-white/80">Memory retention</div>
        <div className="flex items-center gap-2">
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={props.retention.busy || !props.canQuery}
            onClick={() => void props.retention.run()}
          >
            {props.retention.busy ? "Running…" : "Enforce"}
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={props.retention.busy}
            onClick={props.retention.clear}
          >
            Clear
          </button>
        </div>
      </div>
      <div className="grid gap-2 text-[11px] text-white/70">
        <div className="grid grid-cols-2 gap-3">
          <div>
            <FieldLabel>Daily max days</FieldLabel>
            <input
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
              value={props.retention.dailyMaxDays}
              onChange={(e) => props.retention.setDailyMaxDays(e.target.value)}
              inputMode="numeric"
            />
          </div>
          <div>
            <FieldLabel>Daily max bytes</FieldLabel>
            <input
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
              value={props.retention.dailyMaxBytes}
              onChange={(e) => props.retention.setDailyMaxBytes(e.target.value)}
              inputMode="numeric"
            />
          </div>
        </div>
        <div className="grid grid-cols-2 gap-3">
          <div>
            <FieldLabel>Checkpoint max days</FieldLabel>
            <input
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
              value={props.retention.checkpointMaxDays}
              onChange={(e) => props.retention.setCheckpointMaxDays(e.target.value)}
              inputMode="numeric"
            />
          </div>
          <div>
            <FieldLabel>Checkpoint max count</FieldLabel>
            <input
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
              value={props.retention.checkpointMaxCount}
              onChange={(e) => props.retention.setCheckpointMaxCount(e.target.value)}
              inputMode="numeric"
            />
          </div>
        </div>
        <div className="grid grid-cols-2 gap-3">
          <div>
            <FieldLabel>Structured deprecate days</FieldLabel>
            <input
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
              value={props.retention.structuredDeprecateDays}
              onChange={(e) => props.retention.setStructuredDeprecateDays(e.target.value)}
              inputMode="numeric"
            />
          </div>
          <div>
            <FieldLabel>Structured deprecate max entries</FieldLabel>
            <input
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
              value={props.retention.structuredDeprecateMaxEntries}
              onChange={(e) => props.retention.setStructuredDeprecateMaxEntries(e.target.value)}
              inputMode="numeric"
            />
          </div>
        </div>
        <label className="flex items-center gap-2">
          <input type="checkbox" checked={props.retention.dryRun} onChange={(e) => props.retention.setDryRun(e.target.checked)} />
          <span>Dry run (preview only)</span>
        </label>
      </div>
      {props.retention.error ? (
        <div className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
          {props.retention.error}
        </div>
      ) : null}
      {props.retention.result ? (
        <pre className="mt-2 max-h-72 overflow-auto rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/70">
          {props.stringifyJson(props.retention.result)}
        </pre>
      ) : null}
    </section>
  );
}
