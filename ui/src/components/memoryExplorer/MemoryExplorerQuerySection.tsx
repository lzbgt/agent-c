import React from "react";
import { JsonDetails, ToolError, RecordValue } from "../ToolOutput";
import type { MemoryQueryState } from "./useMemoryPanelState";

export default function MemoryExplorerQuerySection({ canQuery, query }: { canQuery: boolean; query: MemoryQueryState }) {
  const { entries: _entries, ...responseMetadata } = query.result || {};
  return <section className="tool-section" data-testid="memory-query-section" aria-label="Saved knowledge">
    <form onSubmit={(event) => { event.preventDefault(); if (!query.busy && canQuery) void query.run(); }}>
      <div className="tool-form-row">
        <label className="tool-field tool-grow">
          Key prefix <span className="tool-muted">(optional)</span>
          <input data-testid="memory-query-prefix-input" value={query.prefix} onChange={(e) => query.setPrefix(e.target.value)} placeholder="e.g. project. — leave empty to browse" />
        </label>
        <button className="workspace-button tool-primary" type="submit" disabled={query.busy || !canQuery}>
          {query.busy ? "Loading…" : "Browse memory"}
        </button>
        {query.result ? <button className="workspace-button" type="button" disabled={query.busy} onClick={query.clear}>Clear</button> : null}
      </div>
      <details className="tool-details" data-testid="memory-query-options">
        <summary>Query options</summary>
        <div className="tool-fields">
          <label className="tool-field">Result limit<input value={query.limit} onChange={(e) => query.setLimit(e.target.value)} inputMode="numeric" /></label>
          <label className="tool-field">Structured path<input value={query.structuredPath} onChange={(e) => query.setStructuredPath(e.target.value)} placeholder="STRUCTURED.md" /></label>
        </div>
      </details>
    </form>
    <ToolError error={query.error} />
    {query.result ? <div className="tool-results" aria-live="polite" aria-busy={query.busy}>
      {query.result.entries.length ? <>
        <p className="tool-muted">{query.result.entries.length} saved {query.result.entries.length === 1 ? "record" : "records"}{query.result.entries.length >= query.result.limit ? " · result limit reached; narrow the prefix or increase the limit" : ""}</p>
        {query.result.entries.map(({ key, record }) => {
          const { value: _value, ...metadata } = record;
          return <article className="tool-card" key={key}>
          <h3>{key}{record.status === "deprecated" ? <span className="tool-muted"> · Deprecated</span> : null}</h3>
          <RecordValue value={record.value} />
          <JsonDetails label="Record details" value={metadata} />
        </article>; })}
      </> : <p className="tool-empty">No saved records match this prefix.</p>}
      <JsonDetails value={responseMetadata} />
    </div> : !query.error && !query.busy ? <p className="tool-empty">Browse saved knowledge, or filter it by the start of a key.</p> : null}
  </section>;
}
