import React from "react";
type Spec = Record<string, any>;
const field = "w-full min-w-0 rounded border border-white/20 bg-black/40 p-2";
export default function ProcessSignal({ spec, onChange }: { spec: Spec; onChange: (next: Spec) => void }) {
  const name = typeof spec.name === "string" ? spec.name : "";
  const validName = /^[A-Za-z0-9_.:-]{1,128}$/.test(name);
  return <div className="min-w-0 space-y-2">
    <label className="block">Signal name<input className={field} value={name} onChange={e => onChange({ ...spec, name: e.target.value })} /></label>
    <label className="block">Signal label<input className={field} value={spec.title || ""} maxLength={256} onChange={e => {
      const next = { ...spec }; if (e.target.value) next.title = e.target.value; else delete next.title; onChange(next);
    }} /></label>
    <label className="block">Signal expiry (milliseconds)<input className={field} type="number" min="0" max="2592000000" step="1" value={spec.timeout_ms ?? 0} onChange={e => {
      if (e.target.value !== "") onChange({ ...spec, timeout_ms: Number(e.target.value) });
    }} /></label>
    <label className="flex items-start gap-2"><input type="checkbox" className="mt-1" checked={spec.accept_before_activation === true} onChange={e => {
      const next = { ...spec }; if (e.target.checked) next.accept_before_activation = true; else delete next.accept_before_activation; onChange(next);
    }} />Accept input before this action is ready</label>
    {spec.accept_before_activation === true ? <p className="text-xs text-white/60">After submission, keep one early input for this exact action. Earlier steps and this action's condition still decide whether it runs. An unused input does not release the process.</p> : null}
    {!validName ? <p role="alert">Choose a signal name using 1–128 letters, digits, dots, colons, underscores or hyphens.</p> : null}
    <p className="text-xs text-white/60">Expiry starts when this action becomes ready; zero waits indefinitely. An authenticated connected system must deliver one JSON result to the run's exact signal identity. Names alone do not route events. Editing does not send input or run the process.</p>
  </div>;
}
