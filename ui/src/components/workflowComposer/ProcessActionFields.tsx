import React from "react";
type Value = Record<string, any>;
const field = "w-full min-w-0 rounded border border-white/20 bg-black/40 p-2";
const button = "rounded border border-white/20 px-2 py-1 disabled:opacity-40";

export function ProcessDeadline({ action, onChange }: { action: Value; onChange: (next: Value) => void }) {
  const due = action.due_unix_ms;
  const valid = Number.isInteger(due) && due > 0 && due <= 253402300799999;
  const value = valid ? new Date(due).toISOString().slice(0, -1) : "";
  return <div className="space-y-2">
    <label className="block">Deadline (UTC)<input aria-label="Deadline (UTC)" className={field}
      type="datetime-local" step="0.001" min="1970-01-01T00:00:00.001" max="9999-12-31T23:59:59.999"
      value={value} onChange={e => {
        const next = { ...action };
        const ms = Date.parse(e.target.value + "Z");
        if (Number.isInteger(ms) && ms > 0 && ms <= 253402300799999) next.due_unix_ms = ms;
        else delete next.due_unix_ms;
        onChange(next);
      }} /></label>
    {!valid ? <p role="alert">Choose a valid UTC deadline before saving or submitting.</p> : <p className="break-words text-xs">Exact due time: {new Date(due).toISOString()}</p>}
    <p className="text-xs text-white/60">This is one fixed UTC instant, not your browser's local time or a recurring schedule. Saved versions keep this deadline. If it is already past when dependencies finish, the action completes immediately.</p>
  </div>;
}

export function ProcessDecision({ decision, onChange }: { decision: Value; onChange: (next: Value) => void }) {
  const choices = Array.isArray(decision.choices) ? decision.choices : [];
  const edit = (index: number, values: Value) => onChange({ ...decision,
    choices: choices.map((c: Value, i: number) => i === index ? { ...c, ...values } : c) });
  return <div className="space-y-2">
    <label className="block">Review question<textarea aria-label="Review question" className={field} value={decision.prompt || ""}
      onChange={e => onChange({ ...decision, prompt: e.target.value })} /></label>
    <label className="block">Decision expiry (milliseconds)<input aria-label="Decision expiry (milliseconds)" className={field}
      type="number" min="0" max="2592000000" step="1" value={decision.timeout_ms ?? 0}
      onChange={e => onChange({ ...decision, timeout_ms: e.target.value === "" ? null : Number(e.target.value) })} /></label>
    <p className="text-xs text-white/60">0 waits indefinitely; otherwise up to 30 days. Expiry starts when this action becomes ready, not when the process is submitted. Expiry fails the action. These choices do not grant tool permissions or person-specific approval.</p>
    {choices.map((choice: Value, i: number) => <fieldset key={i} className="min-w-0 space-y-2 rounded border border-white/10 p-2">
      <legend>Choice {i + 1}</legend>
      <label className="block">Choice label<input aria-label="Choice label" className={field} value={choice.label || ""} maxLength={128} onChange={e => edit(i, { label: e.target.value })} /></label>
      <label className="block">Choice ID<input aria-label="Choice ID" className={field} value={choice.id || ""} maxLength={64} onChange={e => edit(i, { id: e.target.value })} /></label>
      <label className="block">Outcome<select aria-label="Outcome" className={field} value={choice.outcome || ""} onChange={e => edit(i, { outcome: e.target.value })}>
        <option value="" disabled>Choose outcome</option><option value="continue">Continue the process</option><option value="fail">Fail this action</option>
      </select></label>
      <button className={button} type="button" disabled={choices.length <= 1} onClick={() => onChange({ ...decision, choices: choices.filter((_: unknown, n: number) => n !== i) })}>Remove choice</button>
    </fieldset>)}
    <button className={button} type="button" disabled={choices.length >= 8} onClick={() => {
      let n = 1; while (choices.some((c: Value) => c.id === "choice_" + n)) n++;
      onChange({ ...decision, choices: [...choices, { id: "choice_" + n, label: "New choice", outcome: "fail" }] });
    }}>Add choice</button>
    <p className="text-xs text-white/60">1–8 explicit choices. New choices default to failing the action until you deliberately select Continue. IDs must be unique and use letters, digits, dot, colon, underscore or hyphen.</p>
  </div>;
}
