import React from "react";
import type { HistoryPanelTeamState } from "./useHistoryPanelTeamState";

type HistoryPanelTeamSectionProps = {
  teamId: string;
  teamRunId: string;
  teamRunCreatedMs: number;
  teamRunStatus: string;
  teamConversationItems: any[];
  teamConversationWarnings: string[];
  teamState: HistoryPanelTeamState;
  setShowSystemMessages: React.Dispatch<React.SetStateAction<boolean>>;
};

export default function HistoryPanelTeamSection(props: HistoryPanelTeamSectionProps) {
  const {
    teamId,
    teamRunId,
    teamRunCreatedMs,
    teamRunStatus,
    teamConversationItems,
    teamConversationWarnings,
    teamState,
    setShowSystemMessages,
  } = props;
  if (!teamId) return null;

  return (
    <div id="team-chat" className="mb-4 rounded-lg border border-white/10 bg-white/5 px-3 py-2">
      <div className="flex items-center justify-between gap-2">
        <div className="text-sm font-semibold text-white/80">Team chat</div>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
          type="button"
          onClick={() => teamState.setShowTeamChat((value) => !value)}
        >
          {teamState.showTeamChat ? "Hide" : "Show"}
        </button>
      </div>
      {teamState.showTeamChat ? (
        <div className="sticky top-2 z-10 mt-2 rounded-md border border-white/10 bg-black/60 px-2 py-2 backdrop-blur">
          <div className="flex flex-wrap items-center gap-2 text-[11px] text-white/70">
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
              type="button"
              onClick={() => teamState.setTeamCompactMode((value) => !value)}
            >
              {teamState.teamCompactMode ? "Relax" : "Compact"}
            </button>
            <button
              className={`rounded-md border px-2 py-1 text-[11px] ${
                teamState.teamAutoScroll
                  ? "border-emerald-400/30 bg-emerald-500/10 text-emerald-100"
                  : "border-white/10 bg-black/30 text-white/70 hover:bg-black/40"
              }`}
              type="button"
              onClick={() => teamState.setTeamAutoScroll((value) => !value)}
              title={teamState.teamAutoScroll ? "Auto-scroll enabled" : "Auto-scroll disabled"}
            >
              Auto-scroll
            </button>
            <button
              className={`rounded-md border px-2 py-1 text-[11px] ${
                teamState.teamPauseUpdates
                  ? "border-amber-400/30 bg-amber-500/10 text-amber-100"
                  : "border-white/10 bg-black/30 text-white/70 hover:bg-black/40"
              }`}
              type="button"
              onClick={() => {
                if (teamState.teamPauseUpdates) teamState.resumeUpdates();
                else teamState.setTeamPauseUpdates(true);
              }}
            >
              {teamState.teamPauseUpdates ? "Resume" : "Pause"}
            </button>
            <button
              className={`rounded-md border px-2 py-1 text-[11px] ${
                teamState.teamQuietMode
                  ? "border-sky-400/30 bg-sky-500/10 text-sky-100"
                  : "border-white/10 bg-black/30 text-white/70 hover:bg-black/40"
              }`}
              type="button"
              onClick={() => teamState.setTeamQuietMode((value) => !value)}
              title="Quiet mode hides system/tool chatter"
            >
              {teamState.teamQuietMode ? "Quiet ✓" : "Quiet"}
            </button>
            <button
              className={`rounded-md border px-2 py-1 text-[11px] ${
                teamState.teamHideTools
                  ? "border-violet-400/30 bg-violet-500/10 text-violet-100"
                  : "border-white/10 bg-black/30 text-white/70 hover:bg-black/40"
              }`}
              type="button"
              onClick={() => teamState.setTeamHideTools((value) => !value)}
              title="Hide tool messages"
            >
              {teamState.teamHideTools ? "Hide tools" : "Show tools"}
            </button>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
              type="button"
              onClick={teamState.jumpToLatest}
            >
              Latest
            </button>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
              type="button"
              onClick={teamState.openFilters}
            >
              Filters
            </button>
            {teamState.teamSearch.trim().length > 0 ? (
              <>
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                  type="button"
                  onClick={() => teamState.jumpToMatch("first")}
                >
                  First match
                </button>
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                  type="button"
                  onClick={() => teamState.jumpToMatch("last")}
                >
                  Last match
                </button>
              </>
            ) : null}
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
              type="button"
              onClick={() => teamState.setShowTeamHeaders((value) => !value)}
            >
              {teamState.showTeamHeaders ? "Hide meta" : "Show meta"}
            </button>
            <div
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/60"
              title="Search matches message content, role, and agent id. Use Filters for saved/pinned options."
            >
              ?
            </div>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
              type="button"
              onClick={() => {
                const next = !teamState.showTeamSystemMessages;
                teamState.setShowTeamSystemMessages(next);
                setShowSystemMessages(next);
              }}
            >
              {teamState.showTeamSystemMessages ? "Hide system" : "Show system"}
            </button>
            <input
              className="min-w-[180px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80"
              value={teamState.teamSearch}
              onChange={(event) => teamState.setTeamSearch(event.target.value)}
              placeholder="Filter team chat…"
              ref={teamState.teamSearchRef}
              onKeyDown={(event) => {
                if (event.key === "Escape") {
                  teamState.setTeamSearch("");
                }
              }}
            />
            {teamState.teamSearch.trim().length > 0 && !teamState.teamPinnedFilters.includes(teamState.teamSearch.trim()) ? (
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                type="button"
                onClick={() => {
                  const value = teamState.teamSearch.trim();
                  if (!value) return;
                  teamState.setTeamPinnedFilters((prev) => (prev.includes(value) ? prev : [...prev, value]));
                }}
              >
                Pin
              </button>
            ) : null}
            {teamState.teamSearch.trim().length > 0 ? (
              <button
                className={`rounded-md border px-2 py-1 text-[11px] ${
                  teamState.teamDefaultFilter.trim() === teamState.teamSearch.trim()
                    ? "border-emerald-400/30 bg-emerald-500/10 text-emerald-100"
                    : "border-white/10 bg-black/30 text-white/70 hover:bg-black/40"
                }`}
                type="button"
                onClick={() => {
                  const value = teamState.teamSearch.trim();
                  if (!value) return;
                  teamState.setTeamDefaultFilter((prev) => (prev === value ? "" : value));
                }}
              >
                {teamState.teamDefaultFilter.trim() === teamState.teamSearch.trim() ? "Default ✓" : "Set default"}
              </button>
            ) : null}
            {teamState.teamSearch.trim().length > 0 ? (
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                type="button"
                onClick={() => teamState.setTeamSearch("")}
              >
                Clear
              </button>
            ) : null}
          </div>
          {teamState.teamSearch.trim().length > 0 || teamState.teamDefaultFilter.trim().length > 0 ? (
            <div className="mt-2 flex flex-wrap items-center gap-2 text-[10px] text-white/50">
              {teamState.teamSearch.trim().length > 0 ? (
                <span className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-white/70">
                  filter: “{teamState.teamSearch.trim()}”
                </span>
              ) : null}
              {teamState.teamDefaultFilter.trim().length > 0 ? (
                <span className="rounded-md border border-emerald-400/30 bg-emerald-500/10 px-2 py-0.5 text-emerald-100">
                  default: “{teamState.teamDefaultFilter.trim()}”
                </span>
              ) : null}
              {teamState.teamPinnedFilters.length > 0 ? (
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/70 hover:bg-black/40"
                  type="button"
                  onClick={() => teamState.setTeamPinnedFilters([])}
                  title="Clear all pinned filters"
                >
                  Clear pinned
                </button>
              ) : null}
              {teamState.teamPauseUpdates && teamState.teamPendingCount > 0 ? (
                <button
                  className="rounded-md border border-amber-400/30 bg-amber-500/10 px-2 py-0.5 text-amber-100 hover:bg-amber-500/20"
                  type="button"
                  onClick={teamState.resumeUpdates}
                  title="Resume and jump to latest"
                >
                  +{teamState.teamPendingCount} new · jump
                </button>
              ) : null}
            </div>
          ) : null}
          {teamState.teamRoleChips.length > 0 ? (
            <div className="mt-1 flex flex-wrap items-center gap-1 text-[10px] text-white/50">
              {teamState.teamRoleChips.slice(0, 6).map((chip) => (
                <button
                  key={`role-suggest:${chip}`}
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/70 hover:bg-black/40"
                  type="button"
                  onClick={() => teamState.setTeamSearch((prev) => (prev === chip ? "" : chip))}
                >
                  {chip}
                </button>
              ))}
              {teamState.teamRoleChips.length > 6 ? (
                <details className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/70">
                  <summary className="cursor-pointer select-none">+{teamState.teamRoleChips.length - 6} more</summary>
                  <div className="mt-2 flex flex-wrap items-center gap-1">
                    {teamState.teamRoleChips.slice(6).map((chip) => (
                      <button
                        key={`role-suggest-more:${chip}`}
                        className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/70 hover:bg-black/40"
                        type="button"
                        onClick={() => teamState.setTeamSearch((prev) => (prev === chip ? "" : chip))}
                      >
                        {chip}
                      </button>
                    ))}
                  </div>
                </details>
              ) : null}
            </div>
          ) : null}
          <div className="mt-1 text-[10px] text-white/40">Shortcuts: g t (jump), g m (toggle meta), g f (filters), g l (latest)</div>
        </div>
      ) : null}
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
      <details
        id="team-filters"
        className="mt-2 rounded-md border border-white/10 bg-black/20 p-2 text-[11px] text-white/50"
        open={!teamState.teamFiltersCollapsed}
        onToggle={(event) => {
          const open = (event.currentTarget as HTMLDetailsElement).open;
          teamState.setTeamFiltersCollapsed(!open);
        }}
      >
        <summary className="cursor-pointer select-none text-[11px] text-white/70">
          <div className="flex flex-wrap items-center gap-2">
            <span className="font-semibold text-white/80">Filters & controls</span>
            {teamState.filtersActive ? (
              <span className="rounded-md border border-amber-400/30 bg-amber-500/10 px-2 py-0.5 text-[11px] text-amber-100">
                active
              </span>
            ) : null}
            {teamState.teamSearch.trim().length > 0 ? <span className="text-white/50">“{teamState.teamSearch.trim()}”</span> : null}
            {teamState.teamSearch.trim().length > 0 ? (
              <span className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[11px] text-white/70">
                {teamState.teamFilteredCount} match{teamState.teamFilteredCount === 1 ? "" : "es"}
              </span>
            ) : null}
          </div>
        </summary>
        <div className="mt-2 flex flex-wrap items-center gap-2 text-[11px] text-white/50">
          {teamState.teamSavedFilters.length > 0 ? (
            <select
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80"
              value=""
              onChange={(event) => {
                const value = event.target.value;
                if (value) teamState.setTeamSearch(value);
              }}
            >
              <option value="">Saved filters…</option>
              {teamState.teamSavedFilters.map((filter) => (
                <option key={filter} value={filter}>
                  {filter}
                </option>
              ))}
            </select>
          ) : null}
          {teamState.teamSearch.trim().length > 0 ? (
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
              type="button"
              onClick={() => {
                const value = teamState.teamSearch.trim();
                if (!value) return;
                teamState.setTeamSavedFilters((prev) => (prev.includes(value) ? prev : [...prev, value]));
              }}
            >
              Save filter
            </button>
          ) : null}
          {teamState.teamSearch.trim().length > 0 && !teamState.teamPinnedFilters.includes(teamState.teamSearch.trim()) ? (
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
              type="button"
              onClick={() => {
                const value = teamState.teamSearch.trim();
                if (!value) return;
                teamState.setTeamPinnedFilters((prev) => (prev.includes(value) ? prev : [...prev, value]));
              }}
            >
              Pin filter
            </button>
          ) : null}
          {teamState.teamSavedFilters.length > 0 ? (
            <details className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70">
              <summary className="cursor-pointer select-none">Manage saved</summary>
              <div className="mt-2 grid gap-1">
                {teamState.teamSavedFilters.map((filter) => (
                  <div key={`manage-${filter}`} className="flex items-center gap-2">
                    <button
                      className="flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-left text-[11px] text-white/70 hover:bg-black/40"
                      type="button"
                      onClick={() => teamState.setTeamSearch(filter)}
                    >
                      {filter}
                    </button>
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[11px] text-white/70 hover:bg-black/40"
                      type="button"
                      onClick={() =>
                        teamState.setTeamPinnedFilters((prev) =>
                          prev.includes(filter) ? prev.filter((value) => value !== filter) : [...prev, filter],
                        )
                      }
                    >
                      {teamState.teamPinnedFilters.includes(filter) ? "Unpin" : "Pin"}
                    </button>
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[11px] text-white/70 hover:bg-black/40"
                      type="button"
                      onClick={() => teamState.setTeamSavedFilters((prev) => prev.filter((value) => value !== filter))}
                    >
                      Remove
                    </button>
                  </div>
                ))}
                <button
                  className="rounded-md border border-rose-400/30 bg-rose-500/10 px-2 py-0.5 text-left text-[11px] text-rose-100 hover:bg-rose-500/20"
                  type="button"
                  onClick={() => teamState.setTeamSavedFilters([])}
                >
                  Clear all saved filters
                </button>
              </div>
            </details>
          ) : null}
          {teamState.teamSearch.trim().length === 0 ? (
            <span className="text-[11px] text-white/40">Try “executor”, an agent id, or a keyword</span>
          ) : null}
          {teamState.teamPinnedFilters.length > 0 ? (
            <div className="flex flex-wrap items-center gap-1">
              {teamState.teamPinnedFilters.map((chip) => (
                <div key={`pin-${chip}`} className="flex items-center gap-1">
                  <button
                    className="rounded-md border border-emerald-400/30 bg-emerald-500/10 px-2 py-0.5 text-[11px] text-emerald-100 hover:bg-emerald-500/20"
                    type="button"
                    onClick={() => teamState.setTeamSearch(chip)}
                  >
                    {chip}
                  </button>
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-1.5 py-0.5 text-[11px] text-white/60 hover:bg-black/40"
                    type="button"
                    onClick={() => teamState.setTeamPinnedFilters((prev) => prev.filter((value) => value !== chip))}
                    title="Unpin"
                  >
                    ×
                  </button>
                </div>
              ))}
            </div>
          ) : null}
          <div className="flex flex-wrap items-center gap-1">
            {["user", "assistant", "tool"].map((chip) => (
              <button
                key={chip}
                className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[11px] text-white/70 hover:bg-black/40"
                type="button"
                onClick={() => teamState.setTeamSearch((prev) => (prev === chip ? "" : chip))}
              >
                {chip}
              </button>
            ))}
            {teamState.teamRoleChips.map((chip) => (
              <button
                key={`role:${chip}`}
                className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[11px] text-white/70 hover:bg-black/40"
                type="button"
                onClick={() => teamState.setTeamSearch((prev) => (prev === chip ? "" : chip))}
              >
                {chip}
              </button>
            ))}
          </div>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
            type="button"
            onClick={() => teamState.setShowTeamRunMarkers((value) => !value)}
          >
            {teamState.showTeamRunMarkers ? "Hide run markers" : "Show run markers"}
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
            type="button"
            onClick={() => teamState.setShowTeamGroupByAgent((value) => !value)}
          >
            {teamState.showTeamGroupByAgent ? "Timeline view" : "Group by agent"}
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
            type="button"
            onClick={() => teamState.setShowTeamRoleLabels((value) => !value)}
          >
            {teamState.showTeamRoleLabels ? "Hide role labels" : "Show role labels"}
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
            type="button"
            onClick={() => teamState.setShowTeamSystemMessages((value) => !value)}
          >
            {teamState.showTeamSystemMessages ? "Hide system" : "Show system"}
          </button>
          {teamState.teamMutedAgents.length > 0 ? (
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
              type="button"
              onClick={() => teamState.setTeamMutedAgents([])}
            >
              Clear muted
            </button>
          ) : null}
          {teamState.teamSearch.trim().length > 0 ? (
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
              type="button"
              onClick={() => teamState.setTeamSearch("")}
            >
              Clear filter
            </button>
          ) : null}
          {teamState.teamSearch.trim().length > 0 ? (
            <>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                type="button"
                onClick={() => teamState.jumpToMatch("first")}
              >
                First match
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                type="button"
                onClick={() => teamState.jumpToMatch("last")}
              >
                Last match
              </button>
            </>
          ) : null}
          {teamState.filtersActive ? (
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
              type="button"
              onClick={() => {
                teamState.setTeamSearch("");
                teamState.setTeamMutedAgents([]);
                teamState.setShowTeamSystemMessages(false);
                teamState.setShowTeamRunMarkers(true);
              }}
            >
              Clear all
            </button>
          ) : null}
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
            type="button"
            onClick={() => {
              if (typeof window !== "undefined") {
                window.scrollTo({ top: document.body.scrollHeight, behavior: "smooth" });
              }
            }}
          >
            Jump to latest
          </button>
          {teamState.showTeamGroupByAgent && teamState.teamGroupedByAgent.length > 0 ? (
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
              type="button"
              onClick={() => teamState.setTeamGroupState({})}
            >
              Collapse groups
            </button>
          ) : null}
        </div>
      </details>
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
      {teamState.showTeamChat ? (
        teamConversationItems.length === 0 ? (
          <div className="mt-3 rounded-md border border-white/10 bg-black/20 px-3 py-3 text-xs text-white/60">
            No team messages yet. Start a team run to populate this chat.
          </div>
        ) : (
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
        )
      ) : null}
    </div>
  );
}
