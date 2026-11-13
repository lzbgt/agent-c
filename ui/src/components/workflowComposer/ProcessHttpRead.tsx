import React from "react";
type Spec = Record<string, any>;
const field = "w-full min-w-0 rounded border border-white/20 bg-black/40 p-2";
export default function ProcessHttpRead({spec,onChange}:{spec:Spec;onChange:(s:Spec)=>void}) {
 const update=(key:string,value:unknown)=>onChange({...spec,[key]:value});
 const url=typeof spec.url==="string"?spec.url:"";
 let valid=false;
 try { const parsed=new URL(url);valid=["http:","https:"].includes(parsed.protocol)&&!parsed.username&&!parsed.password; } catch {}
 const env=typeof spec.bearer_env==="string"?spec.bearer_env:"";
 return <div className="space-y-2">
  <p>Read JSON from an integration (HTTP GET). Nothing is requested while editing.</p>
  <label className="block">Integration URL<input aria-label="Integration URL" type="url" className={field} value={url} onChange={e=>update("url",e.target.value)} /></label>
  {!valid&&<p role="alert">Enter an HTTP or HTTPS URL without embedded credentials. Server destination policy still applies.</p>}
  <label className="block">Bearer credential environment name<input aria-label="Bearer credential environment name" className={field} value={env} onChange={e=>{const next={...spec};if(e.target.value)next.bearer_env=e.target.value;else delete next.bearer_env;onChange(next);}} /></label>
  {env&&!/^[A-Za-z_][A-Za-z0-9_]{0,127}$/.test(env)&&<p role="alert">Use an environment variable name, not a token.</p>}
  <p className="text-xs text-white/60">Optional: name of a credential already configured on the server. Never paste tokens into this draft or URL. This does not create credentials or enable HTTP tasks.</p>
  <div className="grid gap-2 sm:grid-cols-2">
   <label>Request timeout (milliseconds)<input aria-label="Request timeout (milliseconds)" className={field} type="number" min="1" max="300000" step="1" value={spec.timeout_ms??30000} onChange={e=>update("timeout_ms",e.target.value===""?null:Number(e.target.value))}/></label>
   <label>Response limit (bytes)<input aria-label="Response limit (bytes)" className={field} type="number" min="1024" max="16777216" step="1" value={spec.max_bytes??1048576} onChange={e=>update("max_bytes",e.target.value===""?null:Number(e.target.value))}/></label>
  </div>
  <p className="text-xs text-white/60">GET requests can still disclose the URL and configured credentials to the destination. Save or run only when that destination and use are intended. POST actions remain in advanced configuration with their existing effect/recovery settings intact.</p>
 </div>;
}
