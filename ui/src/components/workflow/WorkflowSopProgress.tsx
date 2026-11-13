import React from "react";
import type { WorkflowDetailResp } from "../../api";
import type { WorkflowTask } from "./workflowPanelUtils";

type Obj = Record<string, unknown>;
const object = (v: unknown): v is Obj => !!v && typeof v === "object" && !Array.isArray(v);
const ids = (v: unknown): v is string[] => Array.isArray(v) && v.length > 0 && v.length <= 128 &&
  v.every(id => typeof id === "string" && id.length > 0) && new Set(v).size === v.length;

export default function WorkflowSopProgress({ detail, tasks }: { detail: WorkflowDetailResp; tasks: WorkflowTask[] }) {
  let spec: unknown = detail.spec;
  if (!spec && detail.spec_json) {
    try { spec = JSON.parse(detail.spec_json); } catch { return null; }
  }
  if (!object(spec) || !object(spec.sop_compiled) || spec.sop_compiled.schema !== "agentd.sop.compiled.v1") return null;
  const manifest = spec.sop_compiled;
  if (!Array.isArray(manifest.stages) || manifest.stages.length > 32) return null;
  const rows = new Map(tasks.map(task => [task.task_id, task]));
  const describe = (taskIds: string[]) => {
    const counts: Record<string, number> = {};
    for (const id of taskIds) {
      const task = rows.get(id);
      const status = !task ? "not loaded" : task.blocker?.required_action ? "needs attention" :
        task.status === "queued" && task.wait?.kind ? "waiting (" + task.wait.kind + ")" : task.status || "unknown";
      counts[status] = (counts[status] || 0) + 1;
    }
    const done = taskIds.filter(id => rows.get(id)?.status === "done").length;
    return done + "/" + taskIds.length + " completed · " +
      Object.entries(counts).map(([status, count]) => count + " " + status).join(" · ");
  };
  return <section aria-label="Process progress" className="min-w-0 rounded-md border border-white/10 bg-black/30 p-3">
    <h2 className="text-sm font-semibold">Process progress</h2>
    <p className="mt-1 break-all text-xs text-white/70">{String(manifest.id ?? "")} · version {String(manifest.version ?? "")}</p>
    <p className="mt-1 text-xs text-white/60">Last loaded task states, not a time estimate. Queued does not mean executing. Reload to refresh.</p>
    <ol className="mt-3 grid min-w-0 gap-3">
      {manifest.stages.map((stage: unknown, index: number) => {
        if (!object(stage) || !ids(stage.task_ids)) return <li key={index}>Stage {index + 1}: progress unavailable</li>;
        const taskIds = stage.task_ids;
        const iterations = Array.isArray(stage.iterations) ? stage.iterations : [];
        return <li key={index} className="min-w-0 rounded border border-white/10 p-2">
          <h3 className="break-words text-sm">{index + 1}. {typeof stage.title === "string" ? stage.title : String(stage.id ?? "Stage")}</h3>
          <p className="break-words text-xs text-white/70">{describe(taskIds)}</p>
          {iterations.length > 0 && iterations.length <= 32 ? <ul className="mt-2 grid gap-1">
            {iterations.map((iteration: unknown, i: number) => <li key={i} className="break-words text-xs text-white/60">
              Repetition {i + 1}: {object(iteration) && ids(iteration.task_ids) &&
                iteration.task_ids.every(id => taskIds.includes(id)) ? describe(iteration.task_ids) : "progress unavailable"}
            </li>)}
          </ul> : null}
        </li>;
      })}
    </ol>
  </section>;
}
