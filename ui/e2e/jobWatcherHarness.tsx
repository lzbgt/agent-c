import React from "react";
import { createRoot } from "react-dom/client";
import useJobStreaming from "../src/hooks/useJobStreaming";
const noop = () => {};
const auth = {token: "synthetic"};
export function mount(fetchMode = false) {
 const node=document.createElement("div");document.body.append(node);
 function Harness(){
  const [active,setActive]=React.useState<string|null>("original");
  const [status,setStatus]=React.useState<string|null>(null);
  const [result,setResult]=React.useState<any>();
  const [notice,setNotice]=React.useState<string|null>(null);
  const [stored,writeStored]=React.useState<any>({scope:{job_id:"original"}});
  useJobStreaming({activeJobId:active,connectionMode:"direct",daemonAuth:auth,
   daemonAuthToken:fetchMode?"synthetic":null,effectiveBase:location.origin,effectiveSseBase:location.origin,
   sessionId:"session",jobStoreKey:"scope",cursorRef:React.useRef(0),lastRunPromptRef:React.useRef("prompt"),
   setActiveJobId:setActive,setJobStatus:setStatus,setJobError:noop,setJobNotice:setNotice,
   setJobUpdatedMs:noop,setLiveEvents:noop,setResult,setLastCompletedPrompt:noop,
   writeJobsBySession:writeStored,auditRefetch:noop,sessionsRefetch:noop});
  (window as any).watchTest={active,status,result,notice,stored,switchJob:()=>setActive("new-job")};
  return React.createElement("div",null,notice);
 }
 createRoot(node).render(React.createElement(Harness));
}
