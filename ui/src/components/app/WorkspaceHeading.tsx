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

export default function WorkspaceHeading(props: { page: string; label: string; sessionId: string; model: string; activeJobId: string }) {
  return <div className="workspace-heading" data-testid="workspace-heading">
    <div><div className="workspace-eyebrow">AGENT WORKSPACE <span>/</span> {props.page ? "CONTROL CENTER" : "OVERVIEW"}</div><h1>{props.label}</h1><p>{descriptions[props.page]}</p></div>
    <div className="workspace-context"><span className="workspace-context-label">CURRENT SESSION</span><span className="workspace-session" title={props.sessionId}>{props.sessionId}</span><span className="workspace-context-detail">{props.activeJobId ? "Run in progress" : "Ready for work"}{props.model ? ` · ${props.model}` : ""}</span></div>
  </div>;
}
