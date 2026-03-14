import React from "react";
import MemoryExplorerIndexSection from "./memoryExplorer/MemoryExplorerIndexSection";
import MemoryExplorerQuerySection from "./memoryExplorer/MemoryExplorerQuerySection";
import MemoryExplorerRecapsSection from "./memoryExplorer/MemoryExplorerRecapsSection";
import MemoryExplorerRetentionSection from "./memoryExplorer/MemoryExplorerRetentionSection";
import { useMemoryPanelState } from "./memoryExplorer/useMemoryPanelState";
import type { ApiAuth } from "../api";

export type MemoryPanelProps = {
  open: boolean;
  onToggle: (open: boolean) => void;
  baseUrl: string;
  auth: ApiAuth;
};

export default function MemoryPanel(props: MemoryPanelProps) {
  const state = useMemoryPanelState({ baseUrl: props.baseUrl, auth: props.auth });

  return (
    <details
      className="mb-4 rounded-lg border border-white/10 bg-white/5 px-3 py-2"
      open={props.open}
      onToggle={(ev) => props.onToggle((ev.currentTarget as HTMLDetailsElement).open)}
      data-testid="memory-panel"
    >
      <summary className="cursor-pointer select-none text-xs text-white/80">
        <div className="flex flex-wrap items-center justify-between gap-2">
          <div className="font-semibold text-white/80">Memory explorer</div>
          <div className="text-[11px] text-white/50">
            Query structured memory + correlate by trace_id + build the correlation index
          </div>
        </div>
      </summary>

      <div className="mt-3 grid gap-4">
        <MemoryExplorerQuerySection
          canQuery={state.canQuery}
          stringifyJson={state.stringifyJson}
          query={state.query}
          correlation={state.correlation}
          checkpoints={state.checkpoints}
        />
        <MemoryExplorerIndexSection
          canQuery={state.canQuery}
          stringifyJson={state.stringifyJson}
          index={state.index}
          salience={state.salience}
        />
        <MemoryExplorerRecapsSection
          canQuery={state.canQuery}
          stringifyJson={state.stringifyJson}
          recaps={state.recaps}
        />
        <MemoryExplorerRetentionSection
          canQuery={state.canQuery}
          stringifyJson={state.stringifyJson}
          retention={state.retention}
        />
      </div>
    </details>
  );
}
