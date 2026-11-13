import React from "react";
type Spec = Record<string, any>;
const field = "w-full min-w-0 rounded border border-white/20 bg-black/40 p-2";
export function editableProcessCommand(spec: Spec) {
 return spec.name === "proc_exec" && spec.arguments && typeof spec.arguments === "object" &&
 !Array.isArray(spec.arguments) && Array.isArray(spec.arguments.argv) &&
 spec.arguments.argv.every((v: unknown) => typeof v === "string");
}
export default function ProcessHostCommand({spec,onChange}:{spec:Spec;onChange:(s:Spec)=>void}) {
 const argv: string[] = spec.arguments.argv;
 const setArgv=(next:string[])=>onChange({...spec,arguments:{...spec.arguments,argv:next}});
 return <div className="space-y-2">
  <p>Run one host command with exact arguments. Editing never runs a command.</p>
  <p className="text-xs text-white/60">Each field is one argument, including spaces and empty strings. No shell parsing is added. A shell executable can still interpret its own arguments. Do not put secrets in this saved process.</p>
  {argv.map((arg,i)=><div key={i} className="flex min-w-0 items-end gap-2">
   <label className="min-w-0 flex-1">{i===0?"Executable":"Argument "+i}<input className={field} aria-label={i===0?"Executable":"Argument "+i} value={arg} onChange={e=>setArgv(argv.map((v,j)=>i===j?e.target.value:v))}/></label>
   {i>0&&<button type="button" aria-label={"Remove argument "+i} className="rounded border border-white/20 p-2" onClick={()=>setArgv(argv.filter((_,j)=>i!==j))}>Remove</button>}
  </div>)}
  {(!argv.length||!argv[0])&&<p role="alert">Choose an executable available on the target host.</p>}
  <button type="button" className="rounded border border-white/20 p-2" onClick={()=>setArgv([...argv,""])}>Add command argument</button>
  <label className="block">Host command timeout (milliseconds)<input className={field} aria-label="Host command timeout (milliseconds)" type="number" min="1" max="300000" step="1" value={spec.timeout_ms??30000} onChange={e=>onChange({...spec,timeout_ms:e.target.value===""?null:Number(e.target.value)})}/></label>
  <p className="text-xs text-white/60">The server must already permit this tool. Existing host policy, approvals and budgets remain mandatory. Saving is not execution approval. An interrupted command may have changed something; inspect its original outcome, never copy it as a blind retry.</p>
 </div>;
}
