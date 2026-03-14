import React from "react";
import type { HistoryPanelTeamState } from "./useHistoryPanelTeamState";

type HistoryPanelTeamTimelineSectionProps = {
  showTeamChat: boolean;
  teamConversationItems: any[];
  teamState: HistoryPanelTeamState;
};

export default function HistoryPanelTeamTimelineSection(props: HistoryPanelTeamTimelineSectionProps) {
  const { showTeamChat, teamConversationItems, teamState } = props;

  if (!showTeamChat) return null;

  if (teamConversationItems.length === 0) {
    return (
      <div className="mt-3 rounded-md border border-white/10 bg-black/20 px-3 py-3 text-xs text-white/60">
        No team messages yet. Start a team run to populate this chat.
      </div>
    );
  }

  return (
    <div className="mt-3 grid gap-3">
      {teamState.teamSearch.trim().length > 0 && teamState.teamFilteredCount === 0 ? (
        <div className="rounded-md border border-amber-400/30 bg-amber-500/10 px-3 py-2 text-[11px] text-amber-100">
          No matches.{" "}
          <button className="underline hover:text-white" type="button" onClick={() => teamState.setTeamSearch("")}>
            Clear filter
          </button>
          .
        </div>
      ) : null}
      {teamState.showTeamGroupByAgent
        ? teamState.teamGroupedByAgent.map((group) => {
            const isMuted = teamState.mutedAgentSet.has(group.key);
            const isOpen = Object.prototype.hasOwnProperty.call(teamState.teamGroupState, group.key)
              ? !!teamState.teamGroupState[group.key]
              : false;
            return (
              <details
                key={group.key}
                className="rounded-md border border-white/10 bg-black/20 p-2"
                open={isOpen}
                onToggle={(event) => {
                  const open = (event.currentTarget as HTMLDetailsElement).open;
                  teamState.setTeamGroupState((prev) => ({ ...(prev || {}), [group.key]: open }));
                }}
              >
                <summary className="cursor-pointer text-[11px] text-white/70">
                  <div className="flex flex-wrap items-center justify-between gap-2">
                    <div className="flex flex-wrap items-center gap-2">
                      <span className="text-white/80">{group.label}</span>
                      <span className="text-white/40">· {group.items.length} messages</span>
                      <span className="text-white/40">· last {group.latest ? new Date(group.latest).toLocaleString() : "unknown"}</span>
                    </div>
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[11px] text-white/70 hover:bg-black/40"
                      type="button"
                      onClick={(event) => {
                        event.preventDefault();
                        event.stopPropagation();
                        teamState.setTeamMutedAgents((prev) =>
                          isMuted ? prev.filter((id) => id !== group.key) : [...prev, group.key],
                        );
                      }}
                    >
                      {isMuted ? "Unmute" : "Mute"}
                    </button>
                  </div>
                  <div className="mt-1 text-[11px] text-white/50">{group.preview}</div>
                </summary>
                <div className="mt-2 grid gap-2">{group.items.map((item, idx) => teamState.renderTeamMessage(item, idx))}</div>
              </details>
            );
          })
        : teamState.teamTimelineItems.map((entry, idx) => {
            if (entry.kind === "marker") {
              return (
                <div
                  key={`team-run:${entry.runId || idx}`}
                  className="rounded-md border border-indigo-400/20 bg-indigo-500/10 px-3 py-2 text-[11px] text-indigo-100"
                >
                  Run {entry.runId}
                </div>
              );
            }
            const visible = teamState.teamVisibleItems.includes(entry.item);
            if (!visible) return null;
            return teamState.renderTeamMessage(entry.item, idx);
          })}
    </div>
  );
}
