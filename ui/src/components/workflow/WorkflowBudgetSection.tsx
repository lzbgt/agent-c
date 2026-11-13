import React from "react";
type Obj = Record<string, unknown>;
const object = (v: unknown): Obj => v !== null && typeof v === "object" && !Array.isArray(v) ? v as Obj : {};
const amount = (v: unknown): number | undefined => typeof v === "number" && Number.isSafeInteger(v) && v >= 0 ? v : undefined;
const show = (v: unknown) => amount(v)?.toLocaleString("en-US") ?? "Unavailable";
const dimensions = [
 ["max_tool_calls_total", "Tool calls"], ["max_steps_total", "Model steps"],
 ["max_elapsed_ms_total", "Execution time (ms)"], ["max_total_tokens", "Token budget"],
];
export default function WorkflowBudgetSection({detail,status,stopped}:{detail:unknown;status?:string;stopped:boolean}) {
 const data=object(detail), limits=object(data.workflow_limits), remaining=object(data.workflow_remaining);
 const ledger=object(data.budget_accounting), charged=object(ledger.charged), held=object(ledger.held), uncertain=object(ledger.uncertain);
 const validLedger=ledger.schema==="agentd.workflow.budget.v1";
 const active=!stopped && (status==="queued"||status==="running");
 const terminal=stopped||["done","error","cancelled"].includes(status??"");
 const results=object(object(data.result).results_by_task);
 const incomplete=Object.keys(results).filter(key=>object(results[key]).token_usage_incomplete===true);
 if(!Object.keys(limits).length&&!Object.keys(ledger).length&&!Object.keys(remaining).length)return null;
 const exhausted=dimensions.filter(([key])=>{const cap=amount(limits[key]),used=amount(charged[key]);return validLedger&&cap!==undefined&&cap>0&&used!==undefined&&used>=cap;});
 const reserved=dimensions.filter(([key])=>validLedger&&amount(remaining[key])===0&&(amount(held[key])??0)>0&&!exhausted.some(([k])=>k===key));
 return <section aria-label="Workflow resources" data-testid="workflow-budgets" className="min-w-0 space-y-3 rounded-md border border-white/10 bg-black/30 p-3 text-sm">
  <h2 className="font-semibold">Workflow resources</h2>
  {terminal&&<p>This run is not accepting new work. Remaining capacity does not resume a stopped or finished run.</p>}
  {!active&&!terminal&&<p>Run status is unavailable. Capacity alone does not establish that work can continue.</p>}
  {!validLedger&&<p role="status">Reservation accounting is unavailable. These values do not establish that new work can run; refresh the original run before making a decision.</p>}
  {!!exhausted.length&&<p role="status" className="text-amber-100">Budget exhausted: {exhausted.map(([,label])=>label).join(", ")}. Inspect the original outcome before considering a new run; a copy can repeat effects.</p>}
  {!!reserved.length&&<p role="status">Capacity reserved by active work: {reserved.map(([,label])=>label).join(", ")}. Wait for the existing attempt to settle; do not start a duplicate.</p>}
  {validLedger&&(amount(ledger.uncertain_attempts)??0)>0&&<p role="status" className="text-amber-100">Outcome uncertain for {show(ledger.uncertain_attempts)} lost attempt(s). Their reservations remain charged, not refunded. Inspect the original receipts and effects; do not replay blindly.</p>}
  {!!incomplete.length&&<p role="status" className="text-amber-100">Provider usage incomplete for {incomplete.slice(0,24).join(", ")}{incomplete.length>24?" and more tasks":""}. The remaining token allowance was conservatively charged. This is not a provider invoice.</p>}
  {validLedger&&<p className="text-xs text-white/60">Active reservations: {show(ledger.active_attempts)}. Uncertain charges below are already included in charged amounts, not added again.</p>}
  <div className="grid min-w-0 gap-2 sm:grid-cols-2">
   {dimensions.map(([key,label])=>{
    const cap=amount(limits[key]);
    const capText=limits[key]===null||cap===0?"Not configured":show(limits[key]);
    return <section key={key} aria-label={label} className="min-w-0 rounded border border-white/10 p-2">
     <h3 className="font-medium">{label}</h3>
     <dl className="mt-1 grid grid-cols-2 gap-x-2 gap-y-1 text-xs">
      <dt>Limit</dt><dd>{capText}</dd>
      <dt>Charged</dt><dd>{validLedger?show(charged[key]):"Unavailable"}</dd>
      <dt>Held for active work</dt><dd>{validLedger?show(held[key]):"Unavailable"}</dd>
      <dt>Uncertain, included</dt><dd>{validLedger?show(uncertain[key]):"Unavailable"}</dd>
      <dt>Available now</dt><dd>{cap!==undefined&&cap>0&&validLedger?show(remaining[key]):"Not established"}</dd>
     </dl>
    </section>;
   })}
  </div>
  <p className="text-xs text-white/60">Charged capacity includes conservative accounting and may differ from actual provider use. Output caps are not a strict total-token ceiling. Permissions and owner stops still apply.</p>
  <details><summary className="cursor-pointer text-xs">Technical budget details</summary><pre className="mt-2 max-h-48 overflow-auto whitespace-pre-wrap break-all text-xs">{JSON.stringify({limits:data.workflow_limits,usage:data.workflow_usage,remaining:data.workflow_remaining,accounting:data.budget_accounting},null,2)}</pre></details>
 </section>;
}
