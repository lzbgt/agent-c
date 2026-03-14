import React from "react";
import type { HistoryPanelTeamState } from "./useHistoryPanelTeamState";

type HistoryPanelTeamFiltersSectionProps = {
  teamState: HistoryPanelTeamState;
};

export default function HistoryPanelTeamFiltersSection({ teamState }: HistoryPanelTeamFiltersSectionProps) {
  return (
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
  );
}
