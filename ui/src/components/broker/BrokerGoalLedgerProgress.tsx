import React from "react";
import type { BrokerOrchestratorRun } from "../../api";
import { goalLedgerReason, record } from "./brokerGoalLedgerUtils";

export default function BrokerGoalLedgerProgress({ run }: { run: BrokerOrchestratorRun | null }) {
  const meta = record(run?.meta), binding = record(meta?.work_ledger);
  if (!binding) return null;
  const runtime = record(meta?.work_ledger_runtime);
  const rows = Array.isArray(runtime?.requirements) ? runtime.requirements.map(record).filter((v): v is Record<string, unknown> => Boolean(v)).slice(0, 256) : [];
  const done = rows.filter((row) => row.status === "done").length;
  const reason = typeof runtime?.reason === "string" ? runtime.reason : "";
  const stopped = run?.status === "paused" || run?.status === "error";
  const label = run?.status === "paused" ? "Paused by owner" : run?.status === "error" ? "Stopped after a failed turn" : goalLedgerReason[reason] || "Awaiting requirement observation";
  return <section className="grid gap-2 rounded border border-sky-400/20 bg-sky-950/20 p-3 text-xs text-white/80" data-testid="goal-ledger-progress">
    <div className="font-semibold">Requirement-driven goal</div>
    <div data-testid="goal-ledger-status">{label}</div>
    <div className="break-all text-[11px] text-white/60">Authority: {String(binding.agent_id || "")} / {String(binding.deployment_id || "")} / {String(binding.session_id || "")}</div>
    {rows.length ? <div data-testid="goal-ledger-count">{done} of {rows.length} requirements verified · observed revision {String(runtime?.revision ?? "?")}</div> : <div>No initialized ledger observed yet. Create requirements through work_state in the authority session.</div>}
    {runtime?.managed_wait === true && !stopped ? <div className="text-[11px] text-white/60">The running coordinator checks this ledger for changes. Unaffected requirements can continue; owner pause always stops continuation.</div> : null}
    {reason === "dispatch_outcome_unknown" ? <div className="text-amber-200">An earlier turn may already have started. Identify its receipt before changing the admission. Changing requirements does not retry it.</div> : null}
    <ul className="grid gap-1">{rows.map((row) => <li key={String(row.id)} data-testid={`goal-requirement-${String(row.id)}`}>
      {row.status === "done" ? "Verified" : row.runnable === true ? "Ready" : "Waiting"}: {String(row.title || row.id)}
    </li>)}</ul>
    <div className="text-[11px] text-white/50">Last observed state, not a live execution guarantee. Verification evidence remains in the authority ledger.</div>
  </section>;
}
