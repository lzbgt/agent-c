import React from "react";
import type { HistoryPanelTeamSectionProps } from "./historyPanelTeamSectionTypes";

export default function HistoryPanelTeamControlsSection(props: HistoryPanelTeamSectionProps) {
  const { teamState, setShowSystemMessages } = props;

  return (
    <>
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
    </>
  );
}
