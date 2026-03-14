import React from "react";
import type { HistoryPanelTeamSectionProps } from "./historyPanelTeamSectionTypes";

export default function HistoryPanelTeamSummarySection(props: HistoryPanelTeamSectionProps) {
  const { teamId, teamRunCreatedMs, teamRunId, teamRunStatus, teamState } = props;

  return (
    <>
      <div className="mt-1 flex flex-wrap items-center gap-2 text-[11px] text-white/50">
        <span className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-white/70">team {teamId}</span>
        {teamRunId ? (
          <span className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-white/60">run {teamRunId}</span>
        ) : null}
        {teamRunStatus ? (
          <span className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-white/60">{teamRunStatus}</span>
        ) : null}
        {teamRunCreatedMs ? (
          <span className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-white/50">
            started {new Date(teamRunCreatedMs).toLocaleString()}
          </span>
        ) : null}
        {teamState.teamAgentSummary.length > 0 ? (
          <span className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-white/50">
            {teamState.teamAgentSummary.length} agents active
          </span>
        ) : null}
        {teamState.teamLastActivity ? (
          <span className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-white/50">
            last activity {new Date(teamState.teamLastActivity).toLocaleString()}
          </span>
        ) : null}
      </div>
      <div className="mt-2 grid gap-2 sm:grid-cols-2">
        <div className="rounded-md border border-white/10 bg-black/20 px-2 py-1">
          <div className="text-[10px] uppercase tracking-wide text-white/40">Run status</div>
          <div className="mt-1 text-[11px] text-white/70">
            {teamRunId ? (
              <>
                <span className="text-white/80">Run {teamRunId}</span>
                {teamRunStatus ? <span className="text-white/50"> · {teamRunStatus}</span> : null}
                {teamRunCreatedMs ? (
                  <span className="text-white/40"> · {new Date(teamRunCreatedMs).toLocaleString()}</span>
                ) : null}
              </>
            ) : (
              <span className="text-white/50">No active run</span>
            )}
          </div>
        </div>
        <div className="rounded-md border border-white/10 bg-black/20 px-2 py-1">
          <div className="text-[10px] uppercase tracking-wide text-white/40">Agents</div>
          <div className="mt-1 text-[11px] text-white/70">
            {teamState.teamAgentSummary.length > 0 ? `${teamState.teamAgentSummary.length} active` : "No agent activity yet"}
          </div>
        </div>
      </div>
      {teamState.teamAgentSummary.length > 0 ? (
        <div className="mt-2 grid gap-2 md:grid-cols-2">
          {teamState.teamAgentSummary
            .slice(0, teamState.showAllTeamAgents ? teamState.teamAgentSummary.length : 4)
            .map((agent) => (
              <div key={agent.agentId} className="rounded-md border border-white/10 bg-black/20 px-2 py-1 text-[11px] text-white/70">
                <div className="flex items-center justify-between gap-2">
                  <span className="text-white/80">{agent.agentId}</span>
                  <span className="text-white/40">{agent.lastTs ? new Date(agent.lastTs).toLocaleString() : "unknown"}</span>
                </div>
                <div className="mt-1 text-white/50">
                  {agent.lastContent.trim().length > 0 ? agent.lastContent.trim().slice(0, 120) : "(no content)"}
                </div>
              </div>
            ))}
          {teamState.teamAgentSummary.length > 4 ? (
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
              type="button"
              onClick={() => teamState.setShowAllTeamAgents((value) => !value)}
            >
              {teamState.showAllTeamAgents ? "Show fewer agents" : `Show all ${teamState.teamAgentSummary.length} agents`}
            </button>
          ) : null}
        </div>
      ) : null}
    </>
  );
}
