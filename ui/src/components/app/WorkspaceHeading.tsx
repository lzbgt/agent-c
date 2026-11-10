import React from "react";

export default function WorkspaceHeading(props: { page: string; label: string; activeJobId: string }) {
  return <div className={`workspace-heading ${props.page ? "" : "conversation-heading"}`} data-testid="workspace-heading">
    <div><h1>{props.page ? props.label : "Conversation"}</h1></div>
    {props.page && props.activeJobId ? <span className="text-xs text-white/50" role="status">Agent working…</span> : null}
  </div>;
}
