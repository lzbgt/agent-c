import React from "react";
import { record } from "./brokerGoalLedgerUtils";

export default function BrokerGoalLedgerBindingForm({ value, onChange }: { value: string; onChange: (next: string) => void }) {
  let meta: Record<string, unknown> | null = null;
  try { meta = value.trim() ? record(JSON.parse(value)) : {}; } catch { /* Keep malformed draft untouched. */ }
  const binding = record(meta?.work_ledger);
  const enabled = Boolean(meta && Object.prototype.hasOwnProperty.call(meta, "work_ledger"));
  const update = (next: Record<string, unknown> | null) => {
    if (!meta) return;
    const result = { ...meta };
    if (next) result.work_ledger = next; else delete result.work_ledger;
    onChange(JSON.stringify(result, null, 2));
  };
  return <fieldset className="grid gap-2 rounded border border-white/10 p-2" data-testid="goal-ledger-binding">
    <label className="flex gap-2 text-xs text-white/80"><input type="checkbox" data-testid="goal-ledger-enabled" checked={enabled} disabled={!meta}
      onChange={(event) => update(event.target.checked ? { agent_id: "", deployment_id: "", session_id: "" } : null)} />Use session requirements to drive this goal</label>
    {enabled ? <>
      <p className="text-[11px] text-white/60">Choose the existing requirement ledger. One eligible team member must match this agent and deployment. Only that authority runs; no automatic role allocation. Team success alone will not finish the goal.</p>
      <div className="grid gap-2 md:grid-cols-3">{(["agent_id", "deployment_id", "session_id"] as const).map((key) =>
        <label key={key} className="grid gap-1 text-[11px] text-white/70">{key.replace("_id", " ID").replace("_", " ")}
          <input data-testid={`goal-ledger-${key}`} value={typeof binding?.[key] === "string" ? String(binding[key]) : ""}
            className="rounded border border-white/10 bg-black/40 px-2 py-1"
            onChange={(event) => update({ ...binding, [key]: event.target.value })} />
        </label>)}</div>
    </> : null}
    {!meta ? <p className="text-[11px] text-amber-200">Fix the Meta JSON before editing the requirement binding.</p> : null}
  </fieldset>;
}
