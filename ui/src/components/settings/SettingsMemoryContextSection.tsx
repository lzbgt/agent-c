import React from "react";
import type { RunSettings } from "../../hooks/useUiSettings";
import FieldLabel from "../FieldLabel";
import { ToggleRow } from "./SettingsControls";

type SettingsMemoryContextSectionProps = {
  run: RunSettings;
};

export default function SettingsMemoryContextSection(props: SettingsMemoryContextSectionProps) {
  const { run } = props;

  return (
    <details className="mt-4 rounded-md border border-white/10 bg-black/20 p-3">
      <summary className="cursor-pointer text-xs font-semibold text-white/70">Memory context</summary>
      <div className="mt-3 grid gap-3 text-[11px] text-white/70">
        <div>
          <FieldLabel>Context mode</FieldLabel>
          <select
            className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
            value={run.memoryContextMode}
            onChange={(e) => run.setMemoryContextMode(e.target.value)}
          >
            <option value="files">files (read memory/*.md)</option>
            <option value="index">index (progressive file index)</option>
            <option value="search">search (ranked snippets)</option>
            <option value="salience">salience (ranked recency/importance)</option>
          </select>
          <div className="mt-1 text-[11px] text-white/40">
            Applies only when tools=host; index shows file size/line/token estimates; search defaults to the prompt if no query is provided.
          </div>
        </div>
        <div className="grid grid-cols-2 gap-3">
          <ToggleRow label="Include structured" checked={run.memoryIncludeStructured} onChange={run.setMemoryIncludeStructured} />
          <ToggleRow label="Include core" checked={run.memoryIncludeCore} onChange={run.setMemoryIncludeCore} />
          <ToggleRow label="Include daily" checked={run.memoryIncludeDaily} onChange={run.setMemoryIncludeDaily} />
          <ToggleRow label="Include session" checked={run.memoryIncludeSession} onChange={run.setMemoryIncludeSession} />
        </div>
        <div className="grid grid-cols-2 gap-3">
          <div>
            <FieldLabel>Daily days</FieldLabel>
            <input
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
              value={run.memoryDailyDays}
              onChange={(e) => run.setMemoryDailyDays(e.target.value)}
              inputMode="numeric"
            />
          </div>
          <div>
            <FieldLabel>Total cap (bytes)</FieldLabel>
            <input
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
              value={run.memoryTotalCap}
              onChange={(e) => run.setMemoryTotalCap(e.target.value)}
              inputMode="numeric"
            />
          </div>
        </div>
        <div>
          <FieldLabel>Search query (optional)</FieldLabel>
          <textarea
            className="mt-1 min-h-[70px] w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
            value={run.memorySearchQuery}
            onChange={(e) => run.setMemorySearchQuery(e.target.value)}
            placeholder="Leave blank to use the current prompt"
          />
        </div>
        <div className="grid grid-cols-2 gap-3">
          <ToggleRow label="Use index (FTS)" checked={run.memorySearchUseIndex} onChange={run.setMemorySearchUseIndex} />
          <ToggleRow label="Case sensitive" checked={run.memorySearchCaseSensitive} onChange={run.setMemorySearchCaseSensitive} />
          <ToggleRow
            label="Fallback to files"
            checked={run.memorySearchFallbackToFiles}
            onChange={run.setMemorySearchFallbackToFiles}
          />
        </div>
        <div>
          <FieldLabel>Order</FieldLabel>
          <select
            className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
            value={run.memorySearchOrder}
            onChange={(e) => run.setMemorySearchOrder(e.target.value)}
          >
            <option value="ranked">Ranked (relevance)</option>
            <option value="newest">Newest first</option>
            <option value="oldest">Oldest first</option>
          </select>
        </div>
        <div className="grid grid-cols-3 gap-3">
          <div>
            <FieldLabel>Max results</FieldLabel>
            <input
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
              value={run.memorySearchMaxResults}
              onChange={(e) => run.setMemorySearchMaxResults(e.target.value)}
              inputMode="numeric"
            />
          </div>
          <div>
            <FieldLabel>Max snippet chars</FieldLabel>
            <input
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
              value={run.memorySearchMaxSnippetChars}
              onChange={(e) => run.setMemorySearchMaxSnippetChars(e.target.value)}
              inputMode="numeric"
            />
          </div>
          <div>
            <FieldLabel>Context lines</FieldLabel>
            <input
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
              value={run.memorySearchContextLines}
              onChange={(e) => run.setMemorySearchContextLines(e.target.value)}
              inputMode="numeric"
            />
          </div>
        </div>
      </div>
    </details>
  );
}
