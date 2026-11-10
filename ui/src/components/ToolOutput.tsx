import React from "react";
import Markdown from "./Markdown";

export function JsonDetails({ label = "Response details", value }: { label?: string; value: unknown }) {
  const [open, setOpen] = React.useState(false);
  return <details className="tool-details" onToggle={(event) => setOpen(event.currentTarget.open)}>
    <summary>{label}</summary>
    {open ? <pre>{JSON.stringify(value, null, 2)}</pre> : null}
  </details>;
}

// The value is the content; provenance, scores and versions belong in details.
export function RecordValue({ value, depth = 0 }: { value: unknown; depth?: number }) {
  if (typeof value === "string") return value ? <Markdown text={value} /> : <p className="tool-muted">Empty value.</p>;
  if (value === null || value === undefined) return <p className="tool-muted">No value recorded.</p>;
  if (typeof value !== "object") return <p>{String(value)}</p>;
  if (depth >= 4) return <p className="tool-muted">Open record details to inspect this nested value.</p>;
  if (Array.isArray(value)) return value.length ? <ul className="tool-values">
    {value.map((item, i) => <li key={i}><RecordValue value={item} depth={depth + 1} /></li>)}
  </ul> : <p className="tool-muted">Empty list.</p>;
  const entries = Object.entries(value);
  return entries.length ? <dl className="tool-values">
    {entries.map(([key, item]) => <div key={key}><dt>{key}</dt><dd><RecordValue value={item} depth={depth + 1} /></dd></div>)}
  </dl> : <p className="tool-muted">Empty record.</p>;
}

export function ToolError({ error }: { error: string | null }) {
  return error ? <p role="alert" className="tool-error">{error}</p> : null;
}
