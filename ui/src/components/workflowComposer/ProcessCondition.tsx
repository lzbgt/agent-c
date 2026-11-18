import React from "react";

type Condition = Record<string, any>;
const field = "w-full min-w-0 rounded border border-white/20 bg-black/40 p-2";

export default function ProcessCondition({ value, sources, taskId, onChange }: {
  value: unknown; sources: string[]; taskId: string; onChange: (next: Condition | undefined) => void;
}) {
  const enabled = value !== undefined;
  const spec: Condition = value && typeof value === "object" && !Array.isArray(value) ? value : {};
  const type = spec.equals === null ? "null" : typeof spec.equals === "boolean" ? "boolean" :
    typeof spec.equals === "string" ? "text" : "unsupported";
  const pointer = typeof spec.pointer === "string" ? spec.pointer : "";
  const valid = sources.includes(spec.task_id) && pointer.startsWith("/") && !pointer.includes("\0") &&
    !/~(?![01])/.test(pointer) && new TextEncoder().encode(pointer).length <= 512 &&
    pointer.split("/").length <= 33 && type !== "unsupported" &&
    (type !== "text" || (!spec.equals.includes("\0") && new TextEncoder().encode(spec.equals).length <= 1024));
  return <div className="min-w-0 space-y-2 rounded border border-white/10 p-2" data-testid={"process-condition-" + taskId}>
    <label className="flex items-start gap-2">
      <input type="checkbox" checked={enabled} disabled={!enabled && sources.length === 0}
        onChange={e => onChange(e.target.checked ? { task_id: sources[0] || "", pointer: "/payload/accepted", equals: true } : undefined)} />
      Run only when a previous result matches
    </label>
    {enabled ? <div className="min-w-0 space-y-2">
      <label className="block">Condition source<select className={field} value={spec.task_id || ""}
        onChange={e => onChange({ ...spec, task_id: e.target.value })}>
        {!sources.includes(spec.task_id) ? <option value={spec.task_id || ""}>Choose an earlier action</option> : null}
        {sources.map(id => <option key={id} value={id}>{id}</option>)}
      </select></label>
      <label className="block">Result field (JSON pointer)<input className={field} value={pointer}
        placeholder="/payload/accepted" onChange={e => onChange({ ...spec, pointer: e.target.value })} /></label>
      <label className="block">Expected value type<select className={field} value={type}
        onChange={e => onChange({ ...spec, equals: e.target.value === "null" ? null : e.target.value === "text" ? "" : true })}>
        <option value="boolean">True or false</option><option value="text">Text</option><option value="null">Explicit null</option>
        {type === "unsupported" ? <option value="unsupported" disabled>Unsupported value</option> : null}
      </select></label>
      {type === "boolean" ? <label className="block">Expected boolean<select className={field} value={String(spec.equals)}
        onChange={e => onChange({ ...spec, equals: e.target.value === "true" })}><option value="true">True</option><option value="false">False</option></select></label> : null}
      {type === "text" ? <label className="block">Expected text<input className={field} value={spec.equals}
        onChange={e => onChange({ ...spec, equals: e.target.value })} /></label> : null}
      {!valid ? <p role="alert">Choose an earlier action, a valid result pointer and a bounded literal value. Missing or wrong-typed results fail closed.</p> : null}
      <p className="text-xs text-white/60">A non-match skips this action without running it. Later actions still follow their own conditions. Missing, failed or skipped source results stop this action. This is not a tool approval.</p>
    </div> : null}
  </div>;
}
