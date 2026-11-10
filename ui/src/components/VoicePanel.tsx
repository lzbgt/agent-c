import React from "react";
import type { ApiAuth } from "../api";
import useVoicePanelState from "./voice/useVoicePanelState";
import { JsonDetails, ToolError } from "./ToolOutput";

export type VoicePanelProps = { baseUrl: string; auth: ApiAuth; sessionId: string };

export default function VoicePanel(props: VoicePanelProps) {
  const [diagnosticsOpen, setDiagnosticsOpen] = React.useState(false);
  const state = useVoicePanelState({ ...props, diagnosticsOpen });
  const action = state.lastActionResponse;
  return <section className="tool-workspace" data-testid="voice-panel" aria-label="Voice controls">
    {!state.canQuery ? <ToolError error="Choose a conversation in Settings to use voice controls." /> : null}
    <section data-testid="voice-control-section">
      <label className="tool-field">Audio source URL
        <input data-testid="voice-panel-source-url" value={state.sourceUrl} onChange={(event) => state.setSourceUrl(event.target.value)} placeholder="Audio URL, or leave empty to resume" />
      </label>
      <div className="tool-fields">
        <label className="tool-field">Volume (0–1)
          <input data-testid="voice-panel-volume" value={state.volumeText} onChange={(event) => state.setVolumeText(event.target.value)} inputMode="decimal" />
        </label>
        <label className="tool-checks"><input checked={state.muted} onChange={(event) => state.setMuted(event.target.checked)} type="checkbox" />Mute audio</label>
      </div>
      <div className="tool-form-row">
        {(["play", "pause", "snapshot"] as const).map((kind) => <button key={kind} data-testid={`voice-panel-${kind}`} className={`workspace-button ${kind === "play" ? "tool-primary" : ""}`} type="button" disabled={!state.canQuery || state.actionMutation.isPending} onClick={() => state.actionMutation.mutate(kind)}>
          {kind === "play" ? "Play" : kind === "pause" ? "Pause" : "Check playback"}
        </button>)}
      </div>
      <ToolError error={state.actionError} />
      {action ? <div role="status" className="mt-4" data-testid="voice-panel-last-action">
        <p>{action.action === "play" ? "Play" : action.action === "pause" ? "Pause" : "Playback check"} request {action.pending_client_execution ? "queued for the browser." : "accepted."}</p>
        <JsonDetails label="Action details" value={action} />
      </div> : null}
      <details className="tool-details" data-testid="voice-playback-options">
        <summary>Playback options</summary>
        <div className="tool-fields">
          <label className="tool-field">Title<input data-testid="voice-panel-title" value={state.title} onChange={(event) => state.setTitle(event.target.value)} /></label>
          <label className="tool-field">Player selector<input data-testid="voice-panel-selector" value={state.selector} onChange={(event) => state.setSelector(event.target.value)} placeholder="#voice-audio" /></label>
        </div>
        <label className="tool-field">Operator note<textarea data-testid="voice-panel-message" className="mt-2 w-full rounded-md border border-white/10 bg-black/30 p-2" value={state.message} onChange={(event) => state.setMessage(event.target.value)} /></label>
        <div className="tool-checks">
          <label><input checked={state.controls} onChange={(event) => state.setControls(event.target.checked)} type="checkbox" />Show player controls</label>
          <label><input checked={state.autoplay} onChange={(event) => state.setAutoplay(event.target.checked)} type="checkbox" />Autoplay</label>
          <label><input checked={state.loop} onChange={(event) => state.setLoop(event.target.checked)} type="checkbox" />Loop</label>
        </div>
      </details>
    </section>
    <details className="tool-details tool-maintenance" data-testid="voice-diagnostics" onToggle={(event) => setDiagnosticsOpen(event.currentTarget.open)}>
      <summary>Playback activity & diagnostics</summary>
      <section data-testid="voice-stats-section">
        <div className="tool-form-row">
          <button data-testid="voice-panel-refresh" className="workspace-button" type="button" disabled={!state.canQuery || state.statsQuery.isFetching} onClick={() => void state.statsQuery.refetch()}>{state.statsQuery.isFetching ? "Loading…" : "Refresh"}</button>
          <label className="tool-checks"><input checked={state.autoRefresh} onChange={(event) => state.setAutoRefresh(event.target.checked)} type="checkbox" />Auto refresh</label>
        </div>
        <ToolError error={state.statsQuery.error instanceof Error ? state.statsQuery.error.message : state.statsQuery.error ? String(state.statsQuery.error) : null} />
        {state.statsQuery.data ? <>
          <p className="tool-muted">{state.statsQuery.data.client_count ?? 0} clients · play {String(state.counts.media_play ?? 0)} · pause {String(state.counts.media_pause ?? 0)} · snapshot {String(state.counts.media_snapshot ?? 0)}</p>
          <JsonDetails label="Clients" value={state.clients} />
          <JsonDetails label="Latest playback result" value={state.statsQuery.data.latest_result} />
          <JsonDetails label="Latest snapshot" value={state.statsQuery.data.latest_snapshot} />
          <JsonDetails label="Recent results" value={state.recentResults} />
          <JsonDetails label="Response details" value={state.statsQuery.data} />
        </> : null}
        <details className="tool-details"><summary>Query options</summary>
          <label className="tool-field">Maximum bytes<input data-testid="voice-panel-max-bytes" value={state.maxBytesText} onChange={(event) => state.setMaxBytesText(event.target.value)} inputMode="numeric" /></label>
        </details>
      </section>
    </details>
  </section>;
}
