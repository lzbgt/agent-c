import React from "react";
import FieldLabel from "../FieldLabel";
import type { BrokerFanoutResult } from "./brokerPanelUtils";

type BrokerMemorySectionProps = {
  agentId: string;
  selectedDeployments: string[];
  retentionDryRun: boolean;
  setRetentionDryRun: (value: boolean) => void;
  retentionDailyMaxDays: string;
  setRetentionDailyMaxDays: (value: string) => void;
  retentionDailyMaxBytes: string;
  setRetentionDailyMaxBytes: (value: string) => void;
  retentionCheckpointMaxDays: string;
  setRetentionCheckpointMaxDays: (value: string) => void;
  retentionCheckpointMaxCount: string;
  setRetentionCheckpointMaxCount: (value: string) => void;
  retentionStructuredDeprecateDays: string;
  setRetentionStructuredDeprecateDays: (value: string) => void;
  retentionStructuredDeprecateMaxEntries: string;
  setRetentionStructuredDeprecateMaxEntries: (value: string) => void;
  retentionBusy: boolean;
  retentionError: string | null;
  retentionResults: BrokerFanoutResult[] | null;
  onRunRetention: () => void;
  onClearRetention: () => void;
  recapsLimit: string;
  setRecapsLimit: (value: string) => void;
  recapsIncludeSummary: boolean;
  setRecapsIncludeSummary: (value: boolean) => void;
  recapsDryRun: boolean;
  setRecapsDryRun: (value: boolean) => void;
  recapsWriteFile: boolean;
  setRecapsWriteFile: (value: boolean) => void;
  recapsKind: string;
  setRecapsKind: (value: string) => void;
  recapsKindFilter: string;
  setRecapsKindFilter: (value: string) => void;
  recapsModel: string;
  setRecapsModel: (value: string) => void;
  recapsSummaryMaxChars: string;
  setRecapsSummaryMaxChars: (value: string) => void;
  recapsDailyDays: string;
  setRecapsDailyDays: (value: string) => void;
  recapsMaxItems: string;
  setRecapsMaxItems: (value: string) => void;
  recapsStructuredMaxItems: string;
  setRecapsStructuredMaxItems: (value: string) => void;
  recapsDailyMaxItems: string;
  setRecapsDailyMaxItems: (value: string) => void;
  recapsHalfLifeDays: string;
  setRecapsHalfLifeDays: (value: string) => void;
  recapsImportanceWeight: string;
  setRecapsImportanceWeight: (value: string) => void;
  recapsIncludeStructured: boolean;
  setRecapsIncludeStructured: (value: boolean) => void;
  recapsIncludeDaily: boolean;
  setRecapsIncludeDaily: (value: boolean) => void;
  recapsListBusy: boolean;
  recapsGenerateBusy: boolean;
  recapsError: string | null;
  recapsResults: BrokerFanoutResult[] | null;
  onRunRecapsList: () => void;
  onRunRecapsGenerate: () => void;
  onClearRecaps: () => void;
  salienceBusy: boolean;
  salienceError: string | null;
  salienceResults: BrokerFanoutResult[] | null;
  onRunSalience: () => void;
  onClearSalience: () => void;
};

export default function BrokerMemorySection(props: BrokerMemorySectionProps) {
  return (
    <section className="rounded-md border border-white/10 bg-black/20 p-3">
      <div className="mb-2 flex items-center justify-between gap-2">
        <div className="text-xs font-semibold text-white/80">Memory maintenance</div>
        <div className="text-[11px] text-white/50">Fan-out retention + recap operations</div>
      </div>

      <div className="grid gap-3">
        <div className="text-xs font-semibold text-white/80">Retention enforce</div>
        <div className="grid grid-cols-1 gap-2 md:grid-cols-2">
          <input className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40" placeholder="daily max days" value={props.retentionDailyMaxDays} onChange={(e) => props.setRetentionDailyMaxDays(e.target.value)} />
          <input className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40" placeholder="daily max bytes" value={props.retentionDailyMaxBytes} onChange={(e) => props.setRetentionDailyMaxBytes(e.target.value)} />
          <input className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40" placeholder="checkpoint max days" value={props.retentionCheckpointMaxDays} onChange={(e) => props.setRetentionCheckpointMaxDays(e.target.value)} />
          <input className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40" placeholder="checkpoint max count" value={props.retentionCheckpointMaxCount} onChange={(e) => props.setRetentionCheckpointMaxCount(e.target.value)} />
          <input className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40" placeholder="structured deprecate days" value={props.retentionStructuredDeprecateDays} onChange={(e) => props.setRetentionStructuredDeprecateDays(e.target.value)} />
          <input className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40" placeholder="structured deprecate max entries" value={props.retentionStructuredDeprecateMaxEntries} onChange={(e) => props.setRetentionStructuredDeprecateMaxEntries(e.target.value)} />
        </div>
        <label className="flex items-center gap-2 text-[11px] text-white/70">
          <input type="checkbox" checked={props.retentionDryRun} onChange={(e) => props.setRetentionDryRun(e.target.checked)} />
          <span>Dry run (preview only)</span>
        </label>
        <div className="flex items-center gap-2">
          <button className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50" type="button" disabled={!props.agentId || props.retentionBusy || props.selectedDeployments.length === 0} onClick={props.onRunRetention}>
            {props.retentionBusy ? "Running…" : "Enforce retention"}
          </button>
          <button className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50" type="button" disabled={props.retentionBusy} onClick={props.onClearRetention}>
            Clear
          </button>
        </div>
        {props.retentionError ? <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">{props.retentionError}</div> : null}
        {props.retentionResults && props.retentionResults.length > 0 ? (
          <div className="grid gap-2">
            {props.retentionResults.map((row) => {
              const depId = String(row?.deployment_id || "");
              const status = row?.status;
              const ok = row?.data?.ok === true;
              const err = row?.data?.error || row?.data?.err;
              const deprecated = row?.data?.structured_deprecated_count;
              return (
                <div key={`retention-${depId}`} className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
                  <div className="text-xs text-white/90">
                    {depId} · {ok ? "ok" : "error"} · http {status}
                  </div>
                  <div className="text-[11px] text-white/50">
                    {typeof deprecated === "number" ? `deprecated ${deprecated}` : "no deprecations"}
                    {err ? ` · ${String(err)}` : ""}
                  </div>
                </div>
              );
            })}
          </div>
        ) : null}
      </div>

      <div className="mt-4 grid gap-3">
        <div className="text-xs font-semibold text-white/80">Recaps</div>
        <div className="grid grid-cols-1 gap-2 md:grid-cols-2">
          <input className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40" placeholder="list limit" value={props.recapsLimit} onChange={(e) => props.setRecapsLimit(e.target.value)} />
          <input className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40" placeholder="summary max chars" value={props.recapsSummaryMaxChars} onChange={(e) => props.setRecapsSummaryMaxChars(e.target.value)} />
          <input className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40" placeholder="recap kind (optional)" value={props.recapsKind} onChange={(e) => props.setRecapsKind(e.target.value)} />
          <input className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40" placeholder="filter kind (list)" value={props.recapsKindFilter} onChange={(e) => props.setRecapsKindFilter(e.target.value)} />
          <input className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40" placeholder="model override (optional)" value={props.recapsModel} onChange={(e) => props.setRecapsModel(e.target.value)} />
          <input className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40" placeholder="daily days" value={props.recapsDailyDays} onChange={(e) => props.setRecapsDailyDays(e.target.value)} />
          <input className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40" placeholder="max items" value={props.recapsMaxItems} onChange={(e) => props.setRecapsMaxItems(e.target.value)} />
          <input className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40" placeholder="structured max items" value={props.recapsStructuredMaxItems} onChange={(e) => props.setRecapsStructuredMaxItems(e.target.value)} />
          <input className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40" placeholder="daily max items" value={props.recapsDailyMaxItems} onChange={(e) => props.setRecapsDailyMaxItems(e.target.value)} />
          <input className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40" placeholder="half-life days" value={props.recapsHalfLifeDays} onChange={(e) => props.setRecapsHalfLifeDays(e.target.value)} />
          <input className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40" placeholder="importance weight" value={props.recapsImportanceWeight} onChange={(e) => props.setRecapsImportanceWeight(e.target.value)} />
        </div>
        <div className="grid grid-cols-1 gap-2 md:grid-cols-2">
          <label className="flex items-center gap-2 text-[11px] text-white/70"><input type="checkbox" checked={props.recapsIncludeSummary} onChange={(e) => props.setRecapsIncludeSummary(e.target.checked)} /><span>Include summary in list</span></label>
          <label className="flex items-center gap-2 text-[11px] text-white/70"><input type="checkbox" checked={props.recapsDryRun} onChange={(e) => props.setRecapsDryRun(e.target.checked)} /><span>Dry run</span></label>
          <label className="flex items-center gap-2 text-[11px] text-white/70"><input type="checkbox" checked={props.recapsWriteFile} onChange={(e) => props.setRecapsWriteFile(e.target.checked)} /><span>Write recap file</span></label>
          <label className="flex items-center gap-2 text-[11px] text-white/70"><input type="checkbox" checked={props.recapsIncludeStructured} onChange={(e) => props.setRecapsIncludeStructured(e.target.checked)} /><span>Include structured</span></label>
          <label className="flex items-center gap-2 text-[11px] text-white/70"><input type="checkbox" checked={props.recapsIncludeDaily} onChange={(e) => props.setRecapsIncludeDaily(e.target.checked)} /><span>Include daily</span></label>
        </div>
        <div className="flex items-center gap-2">
          <button className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50" type="button" disabled={!props.agentId || props.recapsListBusy || props.selectedDeployments.length === 0} onClick={props.onRunRecapsList}>
            {props.recapsListBusy ? "Loading…" : "List recaps"}
          </button>
          <button className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50" type="button" disabled={!props.agentId || props.recapsGenerateBusy || props.selectedDeployments.length === 0} onClick={props.onRunRecapsGenerate}>
            {props.recapsGenerateBusy ? "Generating…" : "Generate recap"}
          </button>
          <button className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50" type="button" disabled={props.recapsListBusy || props.recapsGenerateBusy} onClick={props.onClearRecaps}>
            Clear
          </button>
        </div>
        {props.recapsError ? <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">{props.recapsError}</div> : null}
        {props.recapsResults && props.recapsResults.length > 0 ? (
          <div className="grid gap-2">
            {props.recapsResults.map((row) => {
              const depId = String(row?.deployment_id || "");
              const status = row?.status;
              const ok = row?.data?.ok === true;
              const err = row?.data?.error || row?.data?.err;
              const count = row?.data?.count;
              return (
                <div key={`recaps-${depId}`} className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
                  <div className="text-xs text-white/90">
                    {depId} · {ok ? "ok" : "error"} · http {status}
                  </div>
                  <div className="text-[11px] text-white/50">
                    {typeof count === "number" ? `count ${count}` : "no count"}
                    {err ? ` · ${String(err)}` : ""}
                  </div>
                </div>
              );
            })}
          </div>
        ) : null}
      </div>

      <div className="mt-4 grid gap-3">
        <div className="text-xs font-semibold text-white/80">Salience (uses recaps tuning)</div>
        <div className="text-[11px] text-white/50">Pulls salience with the same limits/weights above.</div>
        <div className="flex items-center gap-2">
          <button className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50" type="button" disabled={!props.agentId || props.salienceBusy || props.selectedDeployments.length === 0} onClick={props.onRunSalience}>
            {props.salienceBusy ? "Fetching…" : "Fetch salience"}
          </button>
          <button className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50" type="button" disabled={props.salienceBusy} onClick={props.onClearSalience}>
            Clear
          </button>
        </div>
        {props.salienceError ? <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">{props.salienceError}</div> : null}
        {props.salienceResults && props.salienceResults.length > 0 ? (
          <div className="grid gap-2">
            {props.salienceResults.map((row) => {
              const depId = String(row?.deployment_id || "");
              const status = row?.status;
              const ok = row?.data?.ok === true;
              const err = row?.data?.error || row?.data?.err;
              const structuredCount = Array.isArray(row?.data?.structured_items) ? row.data.structured_items.length : null;
              const dailyCount = Array.isArray(row?.data?.daily_items) ? row.data.daily_items.length : null;
              const returned = row?.data?.returned;
              return (
                <div key={`salience-${depId}`} className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
                  <div className="text-xs text-white/90">
                    {depId} · {ok ? "ok" : "error"} · http {status}
                  </div>
                  <div className="text-[11px] text-white/50">
                    {typeof returned === "number" ? `returned ${returned}` : "no count"}
                    {structuredCount !== null ? ` · structured ${structuredCount}` : ""}
                    {dailyCount !== null ? ` · daily ${dailyCount}` : ""}
                    {err ? ` · ${String(err)}` : ""}
                  </div>
                </div>
              );
            })}
          </div>
        ) : null}
      </div>
    </section>
  );
}
