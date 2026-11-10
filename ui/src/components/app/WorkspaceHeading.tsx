import React from "react";

const descriptions: Record<string, string> = {
  "": "A clear view of your agents, their work, and what comes next.",
  workflows: "Compose repeatable processes. Follow every stage through execution.",
  memory: "Explore the knowledge your agents carry between runs.",
  trace: "Follow decisions, tool calls, and evidence across a run.",
  approvals: "Review actions that your configured policy sends for human input.",
  voice: "Connect a voice session to your agent workspace.",
  "run-diff": "Compare outcomes and inspect changes between executions.",
  broker: "Coordinate deployments, teams, and autonomous runs.",
};

export default function WorkspaceHeading(props: { page: string; label: string; activeJobId: string }) {
  return <div className={`workspace-heading ${props.page ? "" : "conversation-heading"}`} data-testid="workspace-heading">
    <div><h1>{props.page ? props.label : "Conversation"}</h1>{props.page ? <p>{descriptions[props.page]}</p> : null}</div>
    {props.page && props.activeJobId ? <span className="text-xs text-white/50" role="status">Agent working…</span> : null}
  </div>;
}
