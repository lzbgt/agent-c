import React from "react";
import { apiReconcileWorkflowEffect } from "../../api/workflow";
import type { WorkflowDetailResp } from "../../api/schemas/workflow";
import type { ApiAuth } from "../../api/auth";

type Props = {
  workflowId: string;
  effects: NonNullable<WorkflowDetailResp["effects"]>;
  tasks: Array<{ task_id: string; status?: string; blocker?: { kind?: string } }>;
  controls?: { baseUrl: string; auth?: ApiAuth };
  onReload: (id: string) => void;
};

export default function WorkflowEffectsSection(props: Props) {
  const [urls, setUrls] = React.useState<Record<string,string>>({});
  const [pending, setPending] = React.useState<string | null>(null);
  const [error, setError] = React.useState<string | null>(null);
  const mounted = React.useRef(true);
  React.useEffect(() => { mounted.current = true; return () => { mounted.current = false; }; }, []);
  if (!props.effects.length) return null;

  async function reconcile(taskId: string) {
    if (!props.controls || pending) return;
    setPending(taskId); setError(null);
    try {
      const url = (urls[taskId] ?? "").trim();
      const result = await apiReconcileWorkflowEffect(props.controls.baseUrl, {
        workflow_id: props.workflowId, task_id: taskId, ...(url ? { reconcile_url: url } : {}),
      }, props.controls.auth);
      if (result.ok === false) throw new Error(result.error ?? "Reconciliation could not be queued");
      if (mounted.current) props.onReload(props.workflowId);
    } catch (cause) {
      if (mounted.current) setError(String(cause));
    } finally {
      if (mounted.current) setPending(null);
    }
  }

  return <section className="rounded-lg border border-white/10 bg-black/20 p-3" data-testid="workflow-effects">
    <h3 className="text-xs font-semibold text-white/80">External effects</h3>
    <p className="mt-1 text-[11px] leading-relaxed text-white/50">Durable dispatch and receipt state. Independent tasks can continue while an outcome is reconciled.</p>
    {error ? <p className="mt-2 text-xs text-rose-200" role="alert">{error}</p> : null}
    <div className="mt-3 grid gap-2">
      {props.effects.map((effect) => {
        const task = props.tasks.find((item) => item.task_id === effect.task_id);
        const hostTool = task?.blocker?.kind === "host_tool_outcome_unknown";
        const unresolved = effect.state === "unknown" || (effect.state === "dispatching" && task?.status === "error");
        return <div key={effect.effect_id} className={`rounded-md border p-3 ${unresolved ? "border-amber-300/20 bg-amber-300/5" : "border-white/10 bg-white/[.02]"}`}>
          <div className="flex flex-wrap items-center justify-between gap-2 text-xs">
            <span className="font-medium text-white/80">{effect.task_id}</span>
            <span className={unresolved ? "text-amber-200" : "text-emerald-200"}>{unresolved ? "Outcome unresolved" : effect.state === "completed" ? "Receipt saved" : effect.state === "prepared" ? "Prepared" : "Dispatch admitted"}</span>
          </div>
          <div className="mt-1 text-[11px] text-white/50">{effect.mode === "idempotent" ? "Destination deduplication" : "Guarded write"} · dispatch {effect.attempt}</div>
          {unresolved ? <p className="mt-2 text-[11px] leading-relaxed text-amber-100/80">{hostTool ? "Inspect the original host operation before repeating it. No automatic receipt resolver is available for this tool." : "Verify the destination receipt before repeating this write. Automatic reconciliation follows the task retry policy when a receipt endpoint is configured."}</p> : null}
          {unresolved && !hostTool && task?.status === "error" && props.controls ? <form className="mt-3 flex flex-wrap gap-2" onSubmit={(event) => { event.preventDefault(); void reconcile(effect.task_id); }}>
            <input aria-label={`Receipt endpoint for ${effect.task_id}`} type="url" value={urls[effect.task_id] ?? ""} onChange={(event) => setUrls((prev) => ({ ...prev, [effect.task_id]: event.target.value }))} placeholder="Receipt endpoint (optional if configured)" className="min-w-0 flex-1 rounded border border-white/10 bg-black/20 px-2 py-1 text-xs" />
            <button type="submit" className="workspace-button disabled:opacity-50" disabled={pending !== null}>{pending === effect.task_id ? "Queuing…" : "Reconcile outcome"}</button>
          </form> : null}
          <details className="mt-2 text-[10px] text-white/45"><summary className="cursor-pointer">Effect identity</summary><div className="mt-1 break-all font-mono">{effect.effect_id}<br />{effect.request_hash}</div></details>
        </div>;
      })}
    </div>
  </section>;
}
