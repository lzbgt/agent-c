import React from "react";
import TraceIdTimelineView from "./TraceIdTimelineView";

export type TraceLookupPanelProps = {
  open: boolean;
  onToggle: (open: boolean) => void;
  traceId: string;
  onTraceIdChange: (next: string) => void;
  onLoad: (traceId: string) => void;
  onClear: () => void;
  loading: boolean;
  error: string | null;
  connectionMode: "direct" | "broker";
  baseUrl: string;
  yolo: boolean;
  agentdTrace: any | null;
  brokerTrace: any | null;
};

export default function TraceLookupPanel(props: TraceLookupPanelProps) {
  const traceId = String(props.traceId || "");
  const canLoad = traceId.trim().length > 0;

  return (
    <details
      className="mb-4 rounded-lg border border-white/10 bg-white/5 px-3 py-2"
      open={!!props.open}
      onToggle={(ev) => props.onToggle((ev.currentTarget as HTMLDetailsElement).open)}
    >
      <summary className="cursor-pointer select-none text-xs text-white/80">
        <div className="flex flex-wrap items-center justify-between gap-2">
          <div className="font-semibold text-white/80">Trace lookup</div>
          <div className="text-[11px] text-white/50">
            Debug a distributed run by <code className="text-white/60">trace_id</code>
          </div>
        </div>
      </summary>
      <div className="mt-3 grid gap-3">
        <div className="flex flex-wrap items-center gap-2">
          <input
            className="min-w-[260px] flex-1 rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
            placeholder="trace_id (e.g. trace_...)"
            value={traceId}
            onChange={(e) => props.onTraceIdChange(e.target.value)}
          />
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={props.loading || !canLoad}
            onClick={() => props.onLoad(traceId)}
            title={
              props.connectionMode === "broker"
                ? "Calls broker /v1/trace (includes relay audits + agent fanout)."
                : "Calls agentd /api/v1/trace (audit record search)."
            }
          >
            {props.loading ? "Loading…" : "Load"}
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={props.loading}
            onClick={props.onClear}
          >
            Clear
          </button>
        </div>

        {props.error ? (
          <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200">
            {props.error}
          </div>
        ) : null}

        {traceId.trim() && (props.agentdTrace || props.brokerTrace) ? (
          <TraceIdTimelineView
            mode={props.connectionMode}
            baseUrl={props.baseUrl}
            yolo={props.yolo}
            traceId={traceId.trim()}
            agentdTrace={props.agentdTrace}
            brokerTrace={props.brokerTrace}
          />
        ) : null}
      </div>
    </details>
  );
}
