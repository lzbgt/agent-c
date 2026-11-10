import React from "react";
import Markdown from "../Markdown";
import { JsonDetails, ToolError } from "../ToolOutput";
import MemoryRecapScheduleSection from "./MemoryRecapScheduleSection";
import type { MemoryRecapsState } from "./useMemoryPanelState";

type Props = { canQuery: boolean; stringifyJson: (value: unknown) => string; recaps: MemoryRecapsState };

export default function MemoryExplorerRecapsSection(props: Props) {
  const r = props.recaps;
  const busy = r.listBusy || r.generateBusy;
  const generated = r.result && "input" in r.result ? r.result : null;
  const { recaps: _recaps, summary_text: _summary, prompt: _prompt, structured_items: _structured, daily_items: _daily, ...responseMetadata } = (r.result || {}) as Record<string, unknown>;
  const options: [string, string, (value: string) => void][] = [
    ["Summary character limit", r.summaryMaxChars, r.setSummaryMaxChars],
    ["Model override", r.model, r.setModel],
    ["Summary kind", r.kind, r.setKind],
    ["Max items", r.maxItems, r.setMaxItems],
    ["Structured max items", r.structuredMaxItems, r.setStructuredMaxItems],
    ["Daily max items", r.dailyMaxItems, r.setDailyMaxItems],
    ["Half-life days", r.halfLifeDays, r.setHalfLifeDays],
    ["Importance weight", r.importanceWeight, r.setImportanceWeight],
  ];

  return <section className="tool-section" data-testid="memory-recaps-section" aria-label="Summaries">
    <form onSubmit={(event) => { event.preventDefault(); if (!busy && props.canQuery) void r.runList(); }}>
      <div className="tool-form-row">
        <label className="tool-field tool-grow">Summary kind <span className="tool-muted">(optional)</span>
          <input value={r.kindFilter} onChange={(e) => r.setKindFilter(e.target.value)} placeholder="All kinds, or e.g. daily" />
        </label>
        <button className="workspace-button tool-primary" type="submit" disabled={busy || !props.canQuery}>{r.listBusy ? "Loading…" : "Browse summaries"}</button>
        {r.result ? <button className="workspace-button" type="button" disabled={busy} onClick={r.clear}>Clear</button> : null}
      </div>
    </form>
    <details className="tool-details" data-testid="memory-create-summary">
      <summary>Create a summary</summary>
      <div className="tool-fields">
        <label className="tool-field">Recent days<input value={r.dailyDays} onChange={(e) => r.setDailyDays(e.target.value)} inputMode="numeric" /></label>
        <div className="tool-checks">
          <label><input type="checkbox" checked={r.dryRun} onChange={(e) => r.setDryRun(e.target.checked)} />Preview inputs only</label>
          <label><input type="checkbox" checked={r.writeFile} onChange={(e) => r.setWriteFile(e.target.checked)} disabled={r.dryRun} />Save summary to memory</label>
        </div>
      </div>
      <p className="tool-muted">{r.dryRun ? "Preview selects source records without calling a model or saving a summary." : r.writeFile ? "Generate a summary with the configured model and save it to memory." : "Generate a summary with the configured model without saving a file."}</p>
      <details className="tool-details">
        <summary>Generation options</summary>
        <div className="tool-fields">
          {options.map(([label, value, set]) => <label className="tool-field" key={label}>{label}<input value={value} onChange={(e) => set(e.target.value)} /></label>)}
        </div>
        <div className="tool-checks">
          <label><input type="checkbox" checked={r.includeStructured} onChange={(e) => r.setIncludeStructured(e.target.checked)} />Include structured knowledge</label>
          <label><input type="checkbox" checked={r.includeDaily} onChange={(e) => r.setIncludeDaily(e.target.checked)} />Include daily notes</label>
        </div>
      </details>
      <button className="workspace-button" type="button" disabled={busy || !props.canQuery} onClick={() => void r.runGenerate()}>
        {r.generateBusy ? "Working…" : r.dryRun ? "Preview inputs" : r.writeFile ? "Create & save summary" : "Create summary"}
      </button>
    </details>
    <ToolError error={r.error} />
    <div className="tool-results" aria-live="polite" aria-busy={busy}>
      {generated ? <article className="tool-card">
        <h3>{generated.dry_run ? "Input preview" : generated.recap_path ? "Summary saved" : "Summary created"}</h3>
        {generated.summary_text ? <Markdown text={generated.summary_text} /> : null}
        {generated.dry_run ? <>
          <p className="tool-muted">{generated.input.total_count} source {generated.input.total_count === 1 ? "record" : "records"} selected. Nothing saved.</p>
          {[...generated.structured_items, ...generated.daily_items].map((item, i) => <div className="tool-preview" key={i}>
            {item.key ? <h4>{item.key}</h4> : null}<Markdown text={item.text} />
          </div>)}
        </> : null}
      </article> : r.result ? <>
        {r.filteredList.length ? r.filteredList.map((item) => {
          const { summary_text: _text, ...metadata } = item;
          return <article key={item.recap_path} className="tool-card">
          <h3>{item.kind || "Summary"}{item.ts_utc ? <span className="tool-muted"> · {item.ts_utc}</span> : null}</h3>
          {item.summary_text ? <Markdown text={item.summary_text} /> : <p className="tool-muted">No summary text is available for this snapshot.</p>}
          <JsonDetails label="Summary details" value={metadata} />
        </article>; }) : <p className="tool-empty">No saved summaries match this kind.</p>}
      </> : !r.error && !busy ? <p className="tool-empty">Browse summaries of earlier work, or create one from saved knowledge.</p> : null}
      {r.result ? <JsonDetails value={responseMetadata} /> : null}
    </div>
    <details className="tool-details" data-testid="memory-summary-settings">
      <summary>Summary settings & scheduling</summary>
      <label className="tool-field">List limit<input value={r.limit} onChange={(e) => r.setLimit(e.target.value)} inputMode="numeric" /></label>
      <label className="tool-checks"><input type="checkbox" checked={r.includeSummary} onChange={(e) => r.setIncludeSummary(e.target.checked)} />Include text when browsing summaries</label>
      <MemoryRecapScheduleSection {...props} />
    </details>
  </section>;
}
