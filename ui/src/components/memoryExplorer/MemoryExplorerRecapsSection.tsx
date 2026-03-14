import React from "react";
import FieldLabel from "../FieldLabel";
import type { MemoryRecapsState } from "./useMemoryPanelState";

type MemoryExplorerRecapsSectionProps = {
  canQuery: boolean;
  stringifyJson: (value: unknown) => string;
  recaps: MemoryRecapsState;
};

export default function MemoryExplorerRecapsSection(props: MemoryExplorerRecapsSectionProps) {
  return (
    <section className="rounded-md border border-white/10 bg-black/20 p-3" data-testid="memory-recaps-section">
      <div className="mb-2 flex items-center justify-between gap-2">
        <div className="text-xs font-semibold text-white/80">Memory recaps</div>
        <div className="flex items-center gap-2">
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={props.recaps.listBusy || !props.canQuery}
            onClick={() => void props.recaps.runList()}
          >
            {props.recaps.listBusy ? "Loading…" : "List"}
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={props.recaps.generateBusy || !props.canQuery}
            onClick={() => void props.recaps.runGenerate()}
          >
            {props.recaps.generateBusy ? "Generating…" : "Generate"}
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={props.recaps.listBusy || props.recaps.generateBusy}
            onClick={props.recaps.clear}
          >
            Clear
          </button>
        </div>
      </div>

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

      <div className="mt-3 grid gap-2 text-[11px] text-white/70">
        <div className="grid grid-cols-2 gap-3">
          <div>
            <FieldLabel>List limit</FieldLabel>
            <input
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
              value={props.recaps.limit}
              onChange={(e) => props.recaps.setLimit(e.target.value)}
              inputMode="numeric"
            />
          </div>
          <div>
            <FieldLabel>Summary max chars</FieldLabel>
            <input
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
              value={props.recaps.summaryMaxChars}
              onChange={(e) => props.recaps.setSummaryMaxChars(e.target.value)}
              inputMode="numeric"
            />
          </div>
        </div>
        <div className="grid grid-cols-2 gap-3">
          <div>
            <FieldLabel>Model override (optional)</FieldLabel>
            <input
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
              value={props.recaps.model}
              onChange={(e) => props.recaps.setModel(e.target.value)}
              placeholder="summary model"
            />
          </div>
          <div>
            <FieldLabel>Daily days</FieldLabel>
            <input
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
              value={props.recaps.dailyDays}
              onChange={(e) => props.recaps.setDailyDays(e.target.value)}
              inputMode="numeric"
            />
          </div>
        </div>
        <div className="grid grid-cols-2 gap-3">
          <div>
            <FieldLabel>Recap kind (optional)</FieldLabel>
            <input
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
              value={props.recaps.kind}
              onChange={(e) => props.recaps.setKind(e.target.value)}
              placeholder="daily|weekly|manual"
            />
          </div>
          <div>
            <FieldLabel>Filter kind (list)</FieldLabel>
            <input
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
              value={props.recaps.kindFilter}
              onChange={(e) => props.recaps.setKindFilter(e.target.value)}
              placeholder="daily|weekly|manual"
            />
          </div>
        </div>
        <div className="grid grid-cols-2 gap-3">
          <div>
            <FieldLabel>Max items</FieldLabel>
            <input
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
              value={props.recaps.maxItems}
              onChange={(e) => props.recaps.setMaxItems(e.target.value)}
              inputMode="numeric"
            />
          </div>
          <div>
            <FieldLabel>Structured max items</FieldLabel>
            <input
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
              value={props.recaps.structuredMaxItems}
              onChange={(e) => props.recaps.setStructuredMaxItems(e.target.value)}
              inputMode="numeric"
            />
          </div>
        </div>
        <div className="grid grid-cols-2 gap-3">
          <div>
            <FieldLabel>Daily max items</FieldLabel>
            <input
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
              value={props.recaps.dailyMaxItems}
              onChange={(e) => props.recaps.setDailyMaxItems(e.target.value)}
              inputMode="numeric"
            />
          </div>
          <div>
            <FieldLabel>Half-life days</FieldLabel>
            <input
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
              value={props.recaps.halfLifeDays}
              onChange={(e) => props.recaps.setHalfLifeDays(e.target.value)}
              inputMode="numeric"
            />
          </div>
        </div>
        <div className="grid grid-cols-2 gap-3">
          <div>
            <FieldLabel>Importance weight</FieldLabel>
            <input
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
              value={props.recaps.importanceWeight}
              onChange={(e) => props.recaps.setImportanceWeight(e.target.value)}
              inputMode="numeric"
            />
          </div>
          <label className="flex items-center gap-2 text-[11px] text-white/60">
            <input
              type="checkbox"
              checked={props.recaps.includeSummary}
              onChange={(e) => props.recaps.setIncludeSummary(e.target.checked)}
            />
            <span>Include summary in list</span>
          </label>
        </div>
        <div className="grid grid-cols-2 gap-3">
          <label className="flex items-center gap-2">
            <input type="checkbox" checked={props.recaps.dryRun} onChange={(e) => props.recaps.setDryRun(e.target.checked)} />
            <span>Dry run</span>
          </label>
          <label className="flex items-center gap-2">
            <input type="checkbox" checked={props.recaps.writeFile} onChange={(e) => props.recaps.setWriteFile(e.target.checked)} />
            <span>Write recap file</span>
          </label>
          <label className="flex items-center gap-2">
            <input
              type="checkbox"
              checked={props.recaps.includeStructured}
              onChange={(e) => props.recaps.setIncludeStructured(e.target.checked)}
            />
            <span>Include structured</span>
          </label>
          <label className="flex items-center gap-2">
            <input
              type="checkbox"
              checked={props.recaps.includeDaily}
              onChange={(e) => props.recaps.setIncludeDaily(e.target.checked)}
            />
            <span>Include daily</span>
          </label>
        </div>
      </div>

      {props.recaps.error ? (
        <div className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
          {props.recaps.error}
        </div>
      ) : null}
      {props.recaps.result ? (
        <>
          {props.recaps.list.length > 0 ? (
            <div className="mt-2 grid gap-2 text-[11px] text-white/70">
              <div className="text-[11px] text-white/50">
                Showing {props.recaps.filteredList.length} of {props.recaps.list.length} recap snapshots
              </div>
              {props.recaps.filteredList.map((item: any) => {
                const recapPath = String(item?.recap_path || "");
                const kind = String(item?.kind || "manual");
                const ts = String(item?.ts_utc || "");
                const model = String(item?.model || "");
                const bytes = typeof item?.bytes === "number" ? `${item.bytes} bytes` : "size unknown";
                const summary = String(item?.summary_text || "");
                return (
                  <div key={recapPath || `${kind}-${ts}`} className="rounded-md border border-white/10 bg-black/30 px-2 py-1">
                    <div className="flex flex-wrap items-center justify-between gap-2 text-[11px] text-white/80">
                      <div>
                        <span className="font-semibold">{kind}</span>
                        {ts ? ` · ${ts}` : ""}
                        {model ? ` · ${model}` : ""}
                      </div>
                      <div className="text-white/50">{bytes}</div>
                    </div>
                    <div className="text-[11px] text-white/50">{recapPath || "recap path unknown"}</div>
                    {summary ? <div className="mt-1 text-[11px] text-white/60">{summary}</div> : null}
                  </div>
                );
              })}
            </div>
          ) : null}
          <pre className="mt-2 max-h-72 overflow-auto rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/70">
            {props.stringifyJson(props.recaps.result)}
          </pre>
        </>
      ) : null}
    </section>
  );
}
