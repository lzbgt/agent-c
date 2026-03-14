import React from "react";
import FieldLabel from "../FieldLabel";
import type { MemoryIndexState, MemorySalienceState } from "./useMemoryPanelState";

type MemoryExplorerIndexSectionProps = {
  canQuery: boolean;
  stringifyJson: (value: unknown) => string;
  index: MemoryIndexState;
  salience: MemorySalienceState;
};

export default function MemoryExplorerIndexSection(props: MemoryExplorerIndexSectionProps) {
  return (
    <section className="grid gap-4" data-testid="memory-index-section">
      <section className="rounded-md border border-white/10 bg-black/20 p-3">
        <div className="mb-2 flex items-center justify-between gap-2">
          <div className="text-xs font-semibold text-white/80">Memory index</div>
          <div className="flex items-center gap-2">
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={props.index.busy || !props.canQuery}
              onClick={() => void props.index.run()}
            >
              {props.index.busy ? "Loading…" : "Index"}
            </button>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={props.index.busy}
              onClick={props.index.clear}
            >
              Clear
            </button>
          </div>
        </div>
        <div className="grid gap-2 text-[11px] text-white/70">
          <div className="grid grid-cols-2 gap-3">
            <div>
              <FieldLabel>Session ID (optional)</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                value={props.index.sessionId}
                onChange={(e) => props.index.setSessionId(e.target.value)}
                placeholder="session_..."
              />
            </div>
            <div>
              <FieldLabel>Daily days</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                value={props.index.dailyDays}
                onChange={(e) => props.index.setDailyDays(e.target.value)}
                inputMode="numeric"
              />
            </div>
          </div>
          <div className="grid grid-cols-2 gap-3">
            <label className="flex items-center gap-2">
              <input
                type="checkbox"
                checked={props.index.includeStructured}
                onChange={(e) => props.index.setIncludeStructured(e.target.checked)}
              />
              <span>Include structured</span>
            </label>
            <label className="flex items-center gap-2">
              <input
                type="checkbox"
                checked={props.index.includeCore}
                onChange={(e) => props.index.setIncludeCore(e.target.checked)}
              />
              <span>Include core</span>
            </label>
            <label className="flex items-center gap-2">
              <input
                type="checkbox"
                checked={props.index.includeDaily}
                onChange={(e) => props.index.setIncludeDaily(e.target.checked)}
              />
              <span>Include daily</span>
            </label>
            <label className="flex items-center gap-2">
              <input
                type="checkbox"
                checked={props.index.includeSession}
                onChange={(e) => props.index.setIncludeSession(e.target.checked)}
              />
              <span>Include session</span>
            </label>
          </div>
        </div>
        {props.index.error ? (
          <div className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
            {props.index.error}
          </div>
        ) : null}
        {props.index.result ? (
          <pre className="mt-2 max-h-72 overflow-auto rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/70">
            {props.stringifyJson(props.index.result)}
          </pre>
        ) : null}
      </section>

      <section className="rounded-md border border-white/10 bg-black/20 p-3">
        <div className="mb-2 flex items-center justify-between gap-2">
          <div className="text-xs font-semibold text-white/80">Memory salience</div>
          <div className="flex items-center gap-2">
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={props.salience.busy || !props.canQuery}
              onClick={() => void props.salience.run()}
            >
              {props.salience.busy ? "Loading…" : "Fetch"}
            </button>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={props.salience.busy}
              onClick={props.salience.clear}
            >
              Clear
            </button>
          </div>
        </div>
        <div className="grid gap-2 text-[11px] text-white/70">
          <div className="grid grid-cols-2 gap-3">
            <div>
              <FieldLabel>Daily days</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                value={props.salience.dailyDays}
                onChange={(e) => props.salience.setDailyDays(e.target.value)}
                inputMode="numeric"
              />
            </div>
            <div>
              <FieldLabel>Max items</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                value={props.salience.maxItems}
                onChange={(e) => props.salience.setMaxItems(e.target.value)}
                inputMode="numeric"
              />
            </div>
          </div>
          <div className="grid grid-cols-2 gap-3">
            <div>
              <FieldLabel>Structured max items</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                value={props.salience.structuredMaxItems}
                onChange={(e) => props.salience.setStructuredMaxItems(e.target.value)}
                inputMode="numeric"
              />
            </div>
            <div>
              <FieldLabel>Daily max items</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                value={props.salience.dailyMaxItems}
                onChange={(e) => props.salience.setDailyMaxItems(e.target.value)}
                inputMode="numeric"
              />
            </div>
          </div>
          <div className="grid grid-cols-2 gap-3">
            <div>
              <FieldLabel>Half-life days</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                value={props.salience.halfLifeDays}
                onChange={(e) => props.salience.setHalfLifeDays(e.target.value)}
                inputMode="numeric"
              />
            </div>
            <div>
              <FieldLabel>Importance weight</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                value={props.salience.importanceWeight}
                onChange={(e) => props.salience.setImportanceWeight(e.target.value)}
                inputMode="numeric"
              />
            </div>
          </div>
          <div className="grid grid-cols-2 gap-3">
            <label className="flex items-center gap-2">
              <input
                type="checkbox"
                checked={props.salience.includeStructured}
                onChange={(e) => props.salience.setIncludeStructured(e.target.checked)}
              />
              <span>Include structured</span>
            </label>
            <label className="flex items-center gap-2">
              <input
                type="checkbox"
                checked={props.salience.includeDaily}
                onChange={(e) => props.salience.setIncludeDaily(e.target.checked)}
              />
              <span>Include daily</span>
            </label>
          </div>
        </div>
        {props.salience.error ? (
          <div className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
            {props.salience.error}
          </div>
        ) : null}
        {props.salience.result ? (
          <pre className="mt-2 max-h-72 overflow-auto rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/70">
            {props.stringifyJson(props.salience.result)}
          </pre>
        ) : null}
      </section>
    </section>
  );
}
