import React from "react";
import MemoryExplorerIndexSection from "./memoryExplorer/MemoryExplorerIndexSection";
import MemoryExplorerQuerySection from "./memoryExplorer/MemoryExplorerQuerySection";
import MemoryExplorerRecapsSection from "./memoryExplorer/MemoryExplorerRecapsSection";
import MemoryExplorerRetentionSection from "./memoryExplorer/MemoryExplorerRetentionSection";
import MemoryDiagnosticsSection from "./memoryExplorer/MemoryDiagnosticsSection";
import { useMemoryPanelState } from "./memoryExplorer/useMemoryPanelState";
import type { ApiAuth } from "../api";

export type MemoryPanelProps = {
  baseUrl: string;
  auth: ApiAuth;
};

export default function MemoryPanel(props: MemoryPanelProps) {
  const state = useMemoryPanelState({ baseUrl: props.baseUrl, auth: props.auth });
  const [section, setSection] = React.useState<"knowledge" | "summaries">("knowledge");

  return (
    <section
      aria-label="Memory workspace"
      data-testid="memory-panel"
    >
      <div className="tool-workspace">
        <div className="workspace-tabs" role="group" aria-label="Memory sections">
          <button type="button" aria-pressed={section === "knowledge"} onClick={() => setSection("knowledge")}>Saved knowledge</button>
          <button type="button" aria-pressed={section === "summaries"} onClick={() => setSection("summaries")}>Summaries</button>
        </div>
        <div hidden={section !== "knowledge"}>
          <MemoryExplorerQuerySection canQuery={state.canQuery} query={state.query} />
        </div>
        <div hidden={section !== "summaries"}><MemoryExplorerRecapsSection
          canQuery={state.canQuery}
          stringifyJson={state.stringifyJson}
          recaps={state.recaps}
        /></div>
        <details className="tool-details tool-maintenance" data-testid="memory-advanced">
          <summary>Advanced memory tools</summary>
          <details className="tool-details">
            <summary>Trace correlation & checkpoints</summary>
            <MemoryDiagnosticsSection canQuery={state.canQuery} stringifyJson={state.stringifyJson} correlation={state.correlation} checkpoints={state.checkpoints} />
          </details>
          <details className="tool-details">
            <summary>Index & relevance tuning</summary>
            <MemoryExplorerIndexSection canQuery={state.canQuery} stringifyJson={state.stringifyJson} index={state.index} salience={state.salience} />
          </details>
          <details className="tool-details">
            <summary>Retention & cleanup</summary>
            <MemoryExplorerRetentionSection
              canQuery={state.canQuery}
              retention={state.retention}
            />
          </details>
        </details>
      </div>
    </section>
  );
}
