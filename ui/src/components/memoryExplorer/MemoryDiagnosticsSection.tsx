import React from "react";
import FieldLabel from "../FieldLabel";
import type { MemoryCheckpointsState, MemoryCorrelationState } from "./useMemoryPanelState";

type Props = {
  canQuery: boolean;
  stringifyJson: (value: unknown) => string;
  correlation: MemoryCorrelationState;
  checkpoints: MemoryCheckpointsState;
};

export default function MemoryDiagnosticsSection(props: Props) {
  return <div className="grid gap-4" data-testid="memory-diagnostics-section">
      <section className="rounded-md border border-white/10 bg-black/20 p-3">
        <div className="mb-2 flex items-center justify-between gap-2">
          <div className="text-xs font-semibold text-white/80">Trace correlation</div>
          <div className="flex items-center gap-2">
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={props.correlation.indexBusy || !props.canQuery}
              onClick={() => void props.correlation.buildIndex()}
            >
              {props.correlation.indexBusy ? "Indexing…" : "Build index"}
            </button>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={props.correlation.busy || !props.canQuery}
              onClick={() => void props.correlation.run()}
            >
              {props.correlation.busy ? "Loading…" : "Correlate"}
            </button>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={props.correlation.busy}
              onClick={props.correlation.clear}
            >
              Clear
            </button>
          </div>
        </div>
        <div className="grid gap-2 text-[11px] text-white/70">
          <div className="grid grid-cols-2 gap-3">
            <div>
              <FieldLabel>Trace ID</FieldLabel>
              <input
                data-testid="memory-trace-id-input"
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                value={props.correlation.traceId}
                onChange={(e) => props.correlation.setTraceId(e.target.value)}
                placeholder="trace_..."
              />
            </div>
            <div>
              <FieldLabel>Max entries</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                value={props.correlation.maxEntries}
                onChange={(e) => props.correlation.setMaxEntries(e.target.value)}
                inputMode="numeric"
              />
            </div>
          </div>
          <div className="grid grid-cols-2 gap-3">
            <div>
              <FieldLabel>Key prefix (optional)</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                value={props.correlation.prefix}
                onChange={(e) => props.correlation.setPrefix(e.target.value)}
              />
            </div>
            <div>
              <FieldLabel>Structured path (optional)</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                value={props.correlation.structuredPath}
                onChange={(e) => props.correlation.setStructuredPath(e.target.value)}
                placeholder="STRUCTURED.md"
              />
            </div>
          </div>
          <label className="flex items-center gap-2 text-[11px] text-white/60">
            <input
              type="checkbox"
              checked={props.correlation.timeline}
              onChange={(e) => props.correlation.setTimeline(e.target.checked)}
            />
            <span>Include checkpoint timeline</span>
          </label>
        </div>
        {props.correlation.error ? (
          <div className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
            {props.correlation.error}
          </div>
        ) : null}
        {props.correlation.result ? (
          <pre className="mt-2 max-h-72 overflow-auto rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/70">
            {props.stringifyJson(props.correlation.result)}
          </pre>
        ) : null}
        {props.correlation.indexError ? (
          <div className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
            {props.correlation.indexError}
          </div>
        ) : null}
        {props.correlation.indexResult ? (
          <pre className="mt-2 max-h-60 overflow-auto rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/70">
            {props.stringifyJson(props.correlation.indexResult)}
          </pre>
        ) : null}
      </section>

      <section className="rounded-md border border-white/10 bg-black/20 p-3">
        <div className="mb-2 flex items-center justify-between gap-2">
          <div className="text-xs font-semibold text-white/80">Checkpoints</div>
          <div className="flex items-center gap-2">
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={props.checkpoints.busy || !props.canQuery}
              onClick={() => void props.checkpoints.run()}
            >
              {props.checkpoints.busy ? "Loading…" : "List"}
            </button>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={props.checkpoints.busy}
              onClick={props.checkpoints.clear}
            >
              Clear
            </button>
          </div>
        </div>
        <div className="grid gap-2 text-[11px] text-white/70">
          <div className="grid grid-cols-2 gap-3">
            <div>
              <FieldLabel>Limit</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                value={props.checkpoints.limit}
                onChange={(e) => props.checkpoints.setLimit(e.target.value)}
                inputMode="numeric"
              />
            </div>
            <div>
              <FieldLabel>Structured path (optional)</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                value={props.checkpoints.structuredPath}
                onChange={(e) => props.checkpoints.setStructuredPath(e.target.value)}
                placeholder="STRUCTURED.md"
              />
            </div>
          </div>
        </div>
        {props.checkpoints.error ? (
          <div className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
            {props.checkpoints.error}
          </div>
        ) : null}
        {props.checkpoints.result ? (
          <pre className="mt-2 max-h-72 overflow-auto rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/70">
            {props.stringifyJson(props.checkpoints.result)}
          </pre>
        ) : null}
      </section>
  </div>;
}
