import React from "react";
import type { ModeratorEvent } from "../../api";
import { collectJsonDiffs, formatDiffValue, formatModeratorEventSummary, type JsonDiffEntry } from "./moderatorUtils";

type SettingsModeratorPinsSectionProps = {
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
};

export default function SettingsModeratorPinsSection(props: SettingsModeratorPinsSectionProps) {
  const {
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
  } = props;

  if (moderatorPinnedEntries.length === 0) return null;

  return (
    <div className="mt-2 rounded-md border border-white/10 bg-black/20 p-2">
      <div className="flex items-center justify-between gap-2 text-[11px] text-white/70">
        <span>Pinned events ({moderatorPinnedEntries.length})</span>
        <div className="flex flex-wrap items-center gap-2">
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
            type="button"
            onClick={() => {
              try {
                const payload = JSON.stringify(moderatorPinnedEvents, null, 2);
                const blob = new Blob([payload], { type: "application/json" });
                const url = URL.createObjectURL(blob);
                const anchor = document.createElement("a");
                anchor.href = url;
                anchor.download = `moderator_pins_${Date.now()}.json`;
                anchor.click();
                URL.revokeObjectURL(url);
                showPinNotice("Exported pins", true);
              } catch (err: any) {
                showPinNotice(String(err?.message || "export failed"), false);
              }
            }}
          >
            Export pins
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
            type="button"
            onClick={() => pinImportRef.current?.click()}
          >
            Import pins
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
            type="button"
            onClick={() => updateModeratorPinnedEvents({})}
          >
            Clear pins
          </button>
        </div>
      </div>
      <input
        ref={pinImportRef}
        type="file"
        accept="application/json"
        className="hidden"
        onChange={async (e) => {
          const file = e.target.files?.[0];
          if (!file) return;
          try {
            const text = await file.text();
            const parsed = JSON.parse(text);
            let nextPins: Record<string, ModeratorEvent> = {};
            if (parsed && typeof parsed === "object" && !Array.isArray(parsed)) {
              for (const [key, value] of Object.entries(parsed as Record<string, unknown>)) {
                if (value && typeof value === "object") {
                  nextPins[key] = value as ModeratorEvent;
                }
              }
            } else if (Array.isArray(parsed)) {
              parsed.forEach((event, idx) => {
                if (event && typeof event === "object") {
                  nextPins[`import-${idx}`] = event as ModeratorEvent;
                }
              });
            }
            updateModeratorPinnedEvents(nextPins);
            showPinNotice("Imported pins", true);
          } catch (err: any) {
            showPinNotice(String(err?.message || "import failed"), false);
          } finally {
            if (pinImportRef.current) pinImportRef.current.value = "";
          }
        }}
      />
      <div className="mt-2 grid gap-2">
        {moderatorPinnedEntries.map(([key, event]) => {
          const type = typeof event?.type === "string" ? event.type : "event";
          const ts = typeof event?.ts_unix_ms === "number" ? new Date(event.ts_unix_ms).toLocaleString() : "";
          const actor = event?.actor && typeof event.actor === "object" ? (event.actor as any) : {};
          const actorId = typeof actor?.id === "string" ? actor.id : "";
          const summary = formatModeratorEventSummary(event);
          return (
            <div key={`pinned-${key}`} className="rounded-md border border-white/5 bg-black/30 p-2 text-[11px] text-white/70">
              <div className="flex flex-wrap items-center justify-between gap-2">
                <div className="text-white/80">
                  {type}
                  {actorId ? ` · ${actorId}` : ""}
                </div>
                <div className="text-white/40">{ts}</div>
              </div>
              {summary ? <div className="text-white/60">{summary}</div> : null}
              <div className="mt-1 flex flex-wrap items-center gap-2">
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
                      delete next[key];
                      return next;
                    })
                  }
                >
                  Unpin
                </button>
              </div>
            </div>
          );
        })}
      </div>
      {moderatorPinnedEntries.length > 1 ? (
        <PinnedComparePanel
          moderatorPinnedEvents={moderatorPinnedEvents}
          handleCopy={handleCopy}
          pinnedCompareOptions={pinnedCompareOptions}
          pinnedCompareA={pinnedCompareA}
          setPinnedCompareA={setPinnedCompareA}
          pinnedCompareB={pinnedCompareB}
          setPinnedCompareB={setPinnedCompareB}
          pinnedCompareDiffOnly={pinnedCompareDiffOnly}
          setPinnedCompareDiffOnly={setPinnedCompareDiffOnly}
        />
      ) : null}
    </div>
  );
}

type PinnedComparePanelProps = {
  moderatorPinnedEvents: Record<string, ModeratorEvent>;
  handleCopy: (label: string, text: string) => Promise<void>;
  pinnedCompareOptions: Array<{ key: string; label: string }>;
  pinnedCompareA: string;
  setPinnedCompareA: React.Dispatch<React.SetStateAction<string>>;
  pinnedCompareB: string;
  setPinnedCompareB: React.Dispatch<React.SetStateAction<string>>;
  pinnedCompareDiffOnly: boolean;
  setPinnedCompareDiffOnly: React.Dispatch<React.SetStateAction<boolean>>;
};

function PinnedComparePanel(props: PinnedComparePanelProps) {
  const {
    moderatorPinnedEvents,
    handleCopy,
    pinnedCompareOptions,
    pinnedCompareA,
    setPinnedCompareA,
    pinnedCompareB,
    setPinnedCompareB,
    pinnedCompareDiffOnly,
    setPinnedCompareDiffOnly,
  } = props;

  if (!pinnedCompareA || !pinnedCompareB) return null;
  const eventA = moderatorPinnedEvents[pinnedCompareA];
  const eventB = moderatorPinnedEvents[pinnedCompareB];
  const jsonA = JSON.stringify(eventA, null, 2);
  const jsonB = JSON.stringify(eventB, null, 2);
  const same = jsonA === jsonB;
  const diffText = JSON.stringify({ a: eventA, b: eventB }, null, 2);
  const diffs: JsonDiffEntry[] = [];
  if (!same) {
    collectJsonDiffs(eventA, eventB, "", diffs, 200);
  }

  return (
    <div className="mt-3 rounded-md border border-white/10 bg-black/20 p-2">
      <div className="text-[11px] text-white/70">Compare pinned events</div>
      <div className="mt-2 flex flex-wrap items-center gap-2 text-[11px] text-white/60">
        <label className="flex items-center gap-2">
          <span>A</span>
          <select
            className="min-w-[220px] rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/90"
            value={pinnedCompareA}
            onChange={(e) => setPinnedCompareA(e.target.value)}
          >
            {pinnedCompareOptions.map((opt) => (
              <option key={`pin-a-${opt.key}`} value={opt.key}>
                {opt.label}
              </option>
            ))}
          </select>
        </label>
        <label className="flex items-center gap-2">
          <span>B</span>
          <select
            className="min-w-[220px] rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/90"
            value={pinnedCompareB}
            onChange={(e) => setPinnedCompareB(e.target.value)}
          >
            {pinnedCompareOptions.map((opt) => (
              <option key={`pin-b-${opt.key}`} value={opt.key}>
                {opt.label}
              </option>
            ))}
          </select>
        </label>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
          type="button"
          onClick={() => {
            setPinnedCompareA(pinnedCompareB);
            setPinnedCompareB(pinnedCompareA);
          }}
          disabled={!pinnedCompareA || !pinnedCompareB}
        >
          Swap
        </button>
      </div>
      <div className="mt-2 grid gap-2">
        <div className={`text-[11px] ${same ? "text-emerald-200" : "text-amber-200"}`}>
          {same ? "Pinned events are identical." : "Pinned events differ."}
        </div>
        <label className="flex items-center gap-2 text-[11px] text-white/60">
          <input
            type="checkbox"
            checked={pinnedCompareDiffOnly}
            onChange={(e) => setPinnedCompareDiffOnly(e.target.checked)}
          />
          Diff-only view
        </label>
        <div className="grid gap-2 md:grid-cols-2">
          <div className="rounded-md border border-white/10 bg-black/30 p-2">
            <div className="flex items-center justify-between gap-2 text-[10px] text-white/60">
              <span>Event A</span>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70 hover:bg-black/40"
                type="button"
                onClick={() => void handleCopy("JSON A", jsonA)}
              >
                Copy JSON A
              </button>
            </div>
            {!pinnedCompareDiffOnly ? (
              <pre className="mt-2 max-h-64 overflow-auto whitespace-pre-wrap text-[10px] text-white/70">{jsonA}</pre>
            ) : null}
          </div>
          <div className="rounded-md border border-white/10 bg-black/30 p-2">
            <div className="flex items-center justify-between gap-2 text-[10px] text-white/60">
              <span>Event B</span>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70 hover:bg-black/40"
                type="button"
                onClick={() => void handleCopy("JSON B", jsonB)}
              >
                Copy JSON B
              </button>
            </div>
            {!pinnedCompareDiffOnly ? (
              <pre className="mt-2 max-h-64 overflow-auto whitespace-pre-wrap text-[10px] text-white/70">{jsonB}</pre>
            ) : null}
          </div>
        </div>
        {pinnedCompareDiffOnly ? (
          <div className="rounded-md border border-white/10 bg-black/30 p-2">
            <div className="flex items-center justify-between gap-2 text-[10px] text-white/60">
              <span>Combined diff view</span>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70 hover:bg-black/40"
                type="button"
                onClick={() => void handleCopy("diff JSON", diffText)}
              >
                Copy diff JSON
              </button>
            </div>
            <pre className="mt-2 max-h-64 overflow-auto whitespace-pre-wrap text-[10px] text-white/70">{diffText}</pre>
          </div>
        ) : null}
        {pinnedCompareDiffOnly ? (
          <div className="rounded-md border border-white/10 bg-black/30 p-2">
            <div className="flex items-center justify-between gap-2 text-[10px] text-white/60">
              <span>Key diffs</span>
              <span>{diffs.length} changes</span>
            </div>
            {diffs.length === 0 ? (
              <div className="mt-2 text-[10px] text-white/40">No key-level differences detected.</div>
            ) : (
              <div className="mt-2 max-h-64 overflow-auto">
                {diffs.slice(0, 200).map((diff, idx) => (
                  <div key={`diff-${idx}`} className="border-b border-white/5 py-1 last:border-b-0">
                    <div className="text-[10px] text-white/70">{diff.path}</div>
                    <div className="mt-1 flex flex-wrap items-center gap-2 text-[10px]">
                      <span className="rounded-md bg-emerald-500/10 px-2 py-1 text-emerald-200">
                        A: {formatDiffValue(diff.a)}
                      </span>
                      <span className="rounded-md bg-amber-500/10 px-2 py-1 text-amber-200">
                        B: {formatDiffValue(diff.b)}
                      </span>
                    </div>
                  </div>
                ))}
              </div>
            )}
            {diffs.length >= 200 ? <div className="mt-2 text-[10px] text-white/40">Diffs truncated at 200 entries.</div> : null}
          </div>
        ) : null}
      </div>
    </div>
  );
}
