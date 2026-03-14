import React from "react";
import HistoryPanelTeamControlsSection from "./HistoryPanelTeamControlsSection";
import HistoryPanelTeamFiltersSection from "./HistoryPanelTeamFiltersSection";
import HistoryPanelTeamSummarySection from "./HistoryPanelTeamSummarySection";
import HistoryPanelTeamTimelineSection from "./HistoryPanelTeamTimelineSection";
import type { HistoryPanelTeamSectionProps } from "./historyPanelTeamSectionTypes";

export default function HistoryPanelTeamSection(props: HistoryPanelTeamSectionProps) {
  const { teamId, teamConversationWarnings, teamConversationItems, teamState } = props;
  if (!teamId) return null;

  return (
    <div id="team-chat" className="mb-4 rounded-lg border border-white/10 bg-white/5 px-3 py-2">
      <HistoryPanelTeamControlsSection {...props} />
      <HistoryPanelTeamSummarySection {...props} />
      <HistoryPanelTeamFiltersSection teamState={teamState} />
      {teamState.teamMutedAgents.length > 0 ? (
        <div className="mt-2 flex flex-wrap items-center gap-2 text-[11px] text-white/50">
          Muted:
          {teamState.teamMutedAgents.map((agent) => (
            <button
              key={agent}
              className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[11px] text-white/70 hover:bg-black/40"
              type="button"
              onClick={() => teamState.setTeamMutedAgents((prev) => prev.filter((id) => id !== agent))}
            >
              {agent} ×
            </button>
          ))}
        </div>
      ) : null}
      {teamConversationWarnings.length > 0 ? <div className="mt-2 text-[11px] text-amber-200">{teamConversationWarnings[0]}</div> : null}
      <HistoryPanelTeamTimelineSection
        showTeamChat={teamState.showTeamChat}
        teamConversationItems={teamConversationItems}
        teamState={teamState}
      />
    </div>
  );
}
