import React from "react";

type Obj = Record<string, any>;
const object = (v: unknown): v is Obj => !!v && typeof v === "object" && !Array.isArray(v);
const field = "w-full min-w-0 rounded border border-white/20 bg-black/40 p-2";
const button = "rounded border border-white/20 px-2 py-1 disabled:opacity-40";
export function editableSop(text: string): Obj | null {
  try {
    const root = JSON.parse(text);
    if (!object(root) || !object(root.sop) || root.sop.schema !== "agentd.sop.v1" ||
        !Array.isArray(root.sop.stages) || !root.sop.stages.every((s: unknown) => object(s) &&
        Array.isArray(s.actions) && s.actions.every(object))) return null;
    return root;
  } catch { return null; }
}
export default function WorkflowSopEditor({ value, onChange }: { value: string; onChange: (v: string) => void }) {
  const root = editableSop(value);
  if (!root) return null;
  const sop = root.sop;
  const update = (fn: (draft: Obj) => void) => {
    const draft = JSON.parse(value);
    fn(draft.sop);
    onChange(JSON.stringify(draft, null, 2));
  };
  const unique = (prefix: string, ids: string[]) => {
    let n = 1;
    while (ids.includes(prefix + n)) n++;
    return prefix + n;
  };
  const actionCount = sop.stages.reduce((n: number, s: Obj) => n + s.actions.length, 0);
  return <section className="mt-3 min-w-0 space-y-3 text-sm" aria-label="Process builder">
    <p>Stages run in order. Choose whether actions within each stage run together or one at a time. Editing does not run anything.</p>
    <div className="grid min-w-0 gap-2 sm:grid-cols-3">
      <label>Process name<input className={field} value={sop.title || ""} onChange={e => update(s => { s.title = e.target.value; })} /></label>
      <label>Process ID<input className={field} value={sop.id || ""} onChange={e => update(s => { s.id = e.target.value; })} /></label>
      <label>Version<input className={field} value={sop.version || ""} onChange={e => update(s => { s.version = e.target.value; })} /></label>
    </div>
    <p className="text-xs text-white/60">Saved versions are immutable. Use a new version for changed processes. IDs use letters, digits, underscore or hyphen.</p>
    {sop.stages.map((stage: Obj, i: number) => <fieldset key={i} className="min-w-0 space-y-2 rounded border border-white/15 p-3">
      <legend>Stage {i + 1}</legend>
      <label className="block">Stage name<input className={field} value={stage.title || ""} onChange={e => update(s => { s.stages[i].title = e.target.value; })} /></label>
      <label className="block">Action order<select aria-label="Action order" className={field} value={stage.mode || "sequence"} onChange={e => update(s => { s.stages[i].mode = e.target.value; })}>
        <option value="sequence">One at a time</option><option value="parallel">Together</option>
      </select></label>
      <div className="flex flex-wrap gap-2">
        <button type="button" className={button} disabled={i === 0} onClick={() => update(s => { [s.stages[i - 1], s.stages[i]] = [s.stages[i], s.stages[i - 1]]; })}>Move stage up</button>
        <button type="button" className={button} disabled={i === sop.stages.length - 1} onClick={() => update(s => { [s.stages[i + 1], s.stages[i]] = [s.stages[i], s.stages[i + 1]]; })}>Move stage down</button>
        <button type="button" className={button} disabled={sop.stages.length <= 1} onClick={() => update(s => { s.stages.splice(i, 1); })}>Remove stage</button>
      </div>
      {stage.actions.map((action: Obj, j: number) => <fieldset key={j} className="min-w-0 space-y-2 rounded border border-white/10 p-2">
        <legend className="break-all">Action {j + 1}: {action.task_id}</legend>
        {action.kind === "llm" && object(action.request) ? <label className="block">Instructions<textarea aria-label="Instructions" className={field} value={action.request.prompt || ""} onChange={e => update(s => { s.stages[i].actions[j].request.prompt = e.target.value; })} /></label> :
         action.kind === "delay" ? <label className="block">Wait (milliseconds)<input type="number" min="0" max="600000" className={field} value={action.delay_ms ?? 0} onChange={e => { if (e.target.value !== "") update(s => { s.stages[i].actions[j].delay_ms = Number(e.target.value); }); }} /></label> :
         action.kind === "decision" && object(action.decision) ? <><label className="block">Review question<textarea aria-label="Review question" className={field} value={action.decision.prompt || ""} onChange={e => update(s => { s.stages[i].actions[j].decision.prompt = e.target.value; })} /></label><p className="text-xs">Choices and expiry are preserved; edit them in advanced configuration.</p></> :
         <p className="break-words">Advanced action ({action.kind || "llm"}). Its configuration is preserved unchanged; use advanced configuration to edit it.</p>}
        <div className="flex flex-wrap gap-2">
          <button type="button" className={button} disabled={j === 0} onClick={() => update(s => { const a = s.stages[i].actions; [a[j - 1], a[j]] = [a[j], a[j - 1]]; })}>Move action up</button>
          <button type="button" className={button} disabled={j === stage.actions.length - 1} onClick={() => update(s => { const a = s.stages[i].actions; [a[j + 1], a[j]] = [a[j], a[j + 1]]; })}>Move action down</button>
          <button type="button" className={button} disabled={stage.actions.length <= 1} onClick={() => update(s => { s.stages[i].actions.splice(j, 1); })}>Remove action</button>
        </div>
      </fieldset>)}
      <label className="block">Add action<select aria-label="Add action" className={field} value="" disabled={actionCount >= 128} onChange={e => {
        const kind = e.target.value;
        if (!kind) return;
        update(s => {
          const task_id = unique("action_", s.stages.flatMap((v: Obj) => v.actions.map((a: Obj) => a.task_id)));
          const action = kind === "llm" ? { task_id, kind, request: { prompt: "Describe the work to perform.", tools: "none" } } :
            kind === "decision" ? { task_id, kind, decision: { prompt: "Continue this process?", timeout_ms: 86400000, choices: [
              { id: "continue", label: "Continue", outcome: "continue" }, { id: "stop", label: "Stop", outcome: "fail" },
            ] } } : { task_id, kind: "delay", delay_ms: 0 };
          s.stages[i].actions.push(action);
        });
      }}><option value="">Choose an action…</option><option value="llm">Ask the model</option><option value="delay">Wait</option><option value="decision">Request a decision</option></select></label>
    </fieldset>)}
    <button type="button" className={button} disabled={sop.stages.length >= 32 || actionCount >= 128} onClick={() => update(s => {
      s.stages.push({ id: unique("stage_", s.stages.map((v: Obj) => v.id)), title: "New stage", mode: "sequence", actions: [
        { task_id: unique("action_", s.stages.flatMap((v: Obj) => v.actions.map((a: Obj) => a.task_id))), kind: "delay", delay_ms: 0 },
      ] });
    })}>Add stage</button>
    <p className="text-xs text-white/60">Model actions may use the configured provider and budget. New model actions have tools disabled. Save a version or submit explicitly when ready.</p>
  </section>;
}
