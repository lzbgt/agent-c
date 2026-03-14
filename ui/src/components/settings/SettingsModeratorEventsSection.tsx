import React from "react";
import type { ModeratorEvent } from "../../api";
import { formatModeratorEventSummary } from "./moderatorUtils";
import SettingsModeratorPinsSection from "./SettingsModeratorPinsSection";

type SettingsModeratorEventsSectionProps = {
  sessionId: string;
  moderatorEventsAuto: boolean;
  setModeratorEventsAuto: React.Dispatch<React.SetStateAction<boolean>>;
  moderatorEventsMaxBytes: string;
  setModeratorEventsMaxBytes: React.Dispatch<React.SetStateAction<string>>;
  moderatorEventsIncludeDirectives: boolean;
  setModeratorEventsIncludeDirectives: React.Dispatch<React.SetStateAction<boolean>>;
  moderatorEventsIncludeTasks: boolean;
  setModeratorEventsIncludeTasks: React.Dispatch<React.SetStateAction<boolean>>;
  moderatorEventsFilter: string;
  setModeratorEventsFilter: React.Dispatch<React.SetStateAction<string>>;
  moderatorEventsExpanded: Record<string, boolean>;
  setModeratorEventsExpanded: React.Dispatch<React.SetStateAction<Record<string, boolean>>>;
  moderatorEventsEnabled: boolean;
  moderatorEventsRefetch: () => void;
  moderatorEventsFetching: boolean;
  moderatorEventsError: string | null;
  moderatorEventsList: ModeratorEvent[];
  moderatorEventsFiltered: ModeratorEvent[];
  moderatorPinnedEvents: Record<string, ModeratorEvent>;
  moderatorPinnedEntries: Array<[string, ModeratorEvent]>;
  updateModeratorPinnedEvents: (
    updater: Record<string, ModeratorEvent> | ((prev: Record<string, ModeratorEvent>) => Record<string, ModeratorEvent>),
  ) => void;
  pinImportRef: React.RefObject<HTMLInputElement>;
  showPinNotice: (msg: string, ok: boolean) => void;
  handleCopy: (label: string, text: string) => Promise<void>;
  pinnedCompareOptions: Array<{ key: string; label: string }>;
  pinnedCompareA: string;
  setPinnedCompareA: React.Dispatch<React.SetStateAction<string>>;
  pinnedCompareB: string;
  setPinnedCompareB: React.Dispatch<React.SetStateAction<string>>;
  pinnedCompareDiffOnly: boolean;
  setPinnedCompareDiffOnly: React.Dispatch<React.SetStateAction<boolean>>;
  copyNotice: string | null;
  pinNotice: string | null;
  pinError: string | null;
};

export default function SettingsModeratorEventsSection(props: SettingsModeratorEventsSectionProps) {
  const {
    sessionId,
    moderatorEventsAuto,
    setModeratorEventsAuto,
    moderatorEventsMaxBytes,
    setModeratorEventsMaxBytes,
    moderatorEventsIncludeDirectives,
    setModeratorEventsIncludeDirectives,
    moderatorEventsIncludeTasks,
    setModeratorEventsIncludeTasks,
    moderatorEventsFilter,
    setModeratorEventsFilter,
    moderatorEventsExpanded,
    setModeratorEventsExpanded,
    moderatorEventsEnabled,
    moderatorEventsRefetch,
    moderatorEventsFetching,
    moderatorEventsError,
    moderatorEventsList,
    moderatorEventsFiltered,
    moderatorPinnedEvents,
    moderatorPinnedEntries,
    updateModeratorPinnedEvents,
    pinImportRef,
    showPinNotice,
    handleCopy,
    pinnedCompareOptions,
    pinnedCompareA,
    setPinnedCompareA,
    pinnedCompareB,
    setPinnedCompareB,
    pinnedCompareDiffOnly,
    setPinnedCompareDiffOnly,
    copyNotice,
    pinNotice,
    pinError,
  } = props;

  const moderatorEventsFilterValue = String(moderatorEventsFilter || "").trim().toLowerCase();

  return (
    <div className="mt-3 rounded-md border border-white/10 bg-black/30 p-2">
      <div className="flex flex-wrap items-center justify-between gap-2">
        <div className="text-xs font-semibold text-white/70">Moderator events</div>
        <div className="flex flex-wrap items-center gap-2 text-[11px] text-white/60">
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            onClick={() => moderatorEventsRefetch()}
            disabled={!moderatorEventsEnabled || !sessionId.trim() || moderatorEventsFetching}
          >
            {moderatorEventsFetching ? "Loading…" : "Load"}
          </button>
          <label className="flex items-center gap-2">
            <span>auto</span>
            <input
              type="checkbox"
              checked={moderatorEventsAuto}
              onChange={(e) => setModeratorEventsAuto(e.target.checked)}
              disabled={!moderatorEventsEnabled || !sessionId.trim()}
            />
          </label>
        </div>
      </div>
      <div className="mt-2 flex flex-wrap items-center gap-2 text-[11px] text-white/60">
        <label className="flex items-center gap-2">
          <input
            type="checkbox"
            checked={moderatorEventsIncludeDirectives}
            onChange={(e) => setModeratorEventsIncludeDirectives(e.target.checked)}
          />
          directives
        </label>
        <label className="flex items-center gap-2">
          <input
            type="checkbox"
            checked={moderatorEventsIncludeTasks}
            onChange={(e) => setModeratorEventsIncludeTasks(e.target.checked)}
          />
          tasks
        </label>
        <label className="flex items-center gap-2">
          <span>max bytes</span>
          <input
            className="w-28 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/90"
            value={moderatorEventsMaxBytes}
            onChange={(e) => setModeratorEventsMaxBytes(e.target.value)}
          />
        </label>
        <label className="flex items-center gap-2">
          <span>filter</span>
          <input
            className="w-40 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/90"
            value={moderatorEventsFilter}
            onChange={(e) => setModeratorEventsFilter(e.target.value)}
            placeholder="type/actor/text"
          />
        </label>
        {moderatorEventsFilterValue ? (
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
            type="button"
            onClick={() => setModeratorEventsFilter("")}
          >
            Clear
          </button>
        ) : null}
      </div>
      <SettingsModeratorPinsSection
        moderatorPinnedEvents={moderatorPinnedEvents}
        moderatorPinnedEntries={moderatorPinnedEntries}
        updateModeratorPinnedEvents={updateModeratorPinnedEvents}
        pinImportRef={pinImportRef}
        showPinNotice={showPinNotice}
        handleCopy={handleCopy}
        pinnedCompareOptions={pinnedCompareOptions}
        pinnedCompareA={pinnedCompareA}
        setPinnedCompareA={setPinnedCompareA}
        pinnedCompareB={pinnedCompareB}
        setPinnedCompareB={setPinnedCompareB}
        pinnedCompareDiffOnly={pinnedCompareDiffOnly}
        setPinnedCompareDiffOnly={setPinnedCompareDiffOnly}
      />
      {moderatorEventsError ? <div className="mt-2 text-[11px] text-rose-200">{moderatorEventsError}</div> : null}
      {copyNotice ? <div className="mt-2 text-[11px] text-emerald-200">{copyNotice}</div> : null}
      {pinNotice ? <div className="mt-2 text-[11px] text-emerald-200">{pinNotice}</div> : null}
      {pinError ? <div className="mt-2 text-[11px] text-rose-200">{pinError}</div> : null}
      {!moderatorEventsEnabled ? (
        <div className="mt-2 text-[11px] text-amber-200">Moderator events disabled by daemon caps.</div>
      ) : null}
      {sessionId.trim().length === 0 ? <div className="mt-2 text-[11px] text-amber-200">Set a session id to read events.</div> : null}
      <div className="mt-2 max-h-48 overflow-auto rounded-md border border-white/10 bg-black/20 p-2 text-[11px] text-white/70">
        {moderatorEventsFiltered.length === 0 ? (
          <div className="text-white/40">{moderatorEventsList.length === 0 ? "No events loaded yet." : "No events match the filter."}</div>
        ) : (
          moderatorEventsFiltered.map((event, idx) => {
            const type = typeof event?.type === "string" ? event.type : "event";
            const ts = typeof event?.ts_unix_ms === "number" ? new Date(event.ts_unix_ms).toLocaleString() : "";
            const actor = event?.actor && typeof event.actor === "object" ? (event.actor as any) : {};
            const actorId = typeof actor?.id === "string" ? actor.id : "";
            const summary = formatModeratorEventSummary(event);
            const key = `${type}-${event?.ts_unix_ms ?? "0"}-${idx}`;
            const isPinned = !!moderatorPinnedEvents[key];
            const isExpanded = moderatorEventsExpanded[key] === true;
            return (
              <div key={key} className="border-b border-white/5 py-1 last:border-b-0">
                <div className="flex flex-wrap items-center justify-between gap-2">
                  <div className="text-white/80">
                    {type}
                    {actorId ? ` · ${actorId}` : ""}
                  </div>
                  <div className="flex items-center gap-2 text-white/40">
                    <span>{ts}</span>
                    {summary ? (
                      <button
                        className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70 hover:bg-black/40"
                        type="button"
                        onClick={() => void handleCopy("summary", summary)}
                      >
                        Copy summary
                      </button>
                    ) : null}
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70 hover:bg-black/40"
                      type="button"
                      onClick={() => void handleCopy("JSON", JSON.stringify(event, null, 2))}
                    >
                      Copy JSON
                    </button>
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70 hover:bg-black/40"
                      type="button"
                      onClick={() =>
                        updateModeratorPinnedEvents((prev) => {
                          const next = { ...prev };
                          if (next[key]) {
                            delete next[key];
                          } else {
                            next[key] = event;
                          }
                          return next;
                        })
                      }
                    >
                      {isPinned ? "Unpin" : "Pin"}
                    </button>
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70 hover:bg-black/40"
                      type="button"
                      onClick={() =>
                        setModeratorEventsExpanded((prev) => ({
                          ...prev,
                          [key]: !isExpanded,
                        }))
                      }
                    >
                      {isExpanded ? "Hide JSON" : "Show JSON"}
                    </button>
                  </div>
                </div>
                {summary ? <div className="text-white/60">{summary}</div> : null}
                {isExpanded ? (
                  <pre className="mt-2 max-h-64 overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/40 p-2 text-[10px] text-white/70">
                    {JSON.stringify(event, null, 2)}
                  </pre>
                ) : null}
              </div>
            );
          })
        )}
      </div>
    </div>
  );
}
