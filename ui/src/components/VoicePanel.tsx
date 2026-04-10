import React from "react";
import type { ApiAuth } from "../api";
import useVoicePanelState from "./voice/useVoicePanelState";

export type VoicePanelProps = {
  open: boolean;
  onToggle: (open: boolean) => void;
  baseUrl: string;
  auth: ApiAuth;
  sessionId: string;
};

function renderJson(value: unknown): string {
  try {
    return JSON.stringify(value ?? null, null, 2);
  } catch {
    return String(value ?? "");
  }
}

export default function VoicePanel(props: VoicePanelProps) {
  const state = useVoicePanelState({
    auth: props.auth,
    baseUrl: props.baseUrl,
    sessionId: props.sessionId,
  });

  return (
    <details
      className="mb-4 rounded-lg border border-white/10 bg-white/5 px-3 py-2"
      open={props.open}
      onToggle={(event) => props.onToggle((event.currentTarget as HTMLDetailsElement).open)}
      data-testid="voice-panel"
    >
      <summary className="cursor-pointer select-none text-xs text-white/80">
        <div className="flex flex-wrap items-center justify-between gap-2">
          <div className="font-semibold text-white/80">Voice control</div>
          <div className="text-[11px] text-white/50">Drive session media RPCs and inspect durable voice stats</div>
        </div>
      </summary>

      <div className="mt-3 grid gap-4">
        {!state.canQuery ? (
          <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-3 py-2 text-[11px] text-amber-100">
            Missing base URL or session ID.
          </div>
        ) : null}

        <section className="rounded-md border border-white/10 bg-black/20 p-3" data-testid="voice-control-section">
          <div className="mb-3 flex flex-wrap items-center justify-between gap-2">
            <div>
              <div className="text-xs font-semibold text-white/80">Session controls</div>
              <div className="text-[11px] text-white/50">Persist server-owned play, pause, and snapshot actions for this session.</div>
            </div>
            <div className="text-[11px] text-white/50">session {state.sessionId || "default"}</div>
          </div>

          <div className="grid gap-3 md:grid-cols-2">
            <label className="block text-[11px] text-white/60">
              Selector
              <input
                data-testid="voice-panel-selector"
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white"
                value={state.selector}
                onChange={(event) => state.setSelector(event.target.value)}
                placeholder="#voice-audio"
              />
            </label>
            <label className="block text-[11px] text-white/60">
              Source URL
              <input
                data-testid="voice-panel-source-url"
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white"
                value={state.sourceUrl}
                onChange={(event) => state.setSourceUrl(event.target.value)}
                placeholder="https://example.invalid/voice.wav or data:audio/..."
              />
            </label>
            <label className="block text-[11px] text-white/60">
              Title
              <input
                data-testid="voice-panel-title"
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white"
                value={state.title}
                onChange={(event) => state.setTitle(event.target.value)}
                placeholder="Voice output"
              />
            </label>
            <label className="block text-[11px] text-white/60">
              Volume
              <input
                data-testid="voice-panel-volume"
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white"
                value={state.volumeText}
                onChange={(event) => state.setVolumeText(event.target.value)}
                placeholder="1"
              />
            </label>
          </div>

          <label className="mt-3 block text-[11px] text-white/60">
            Message
            <textarea
              data-testid="voice-panel-message"
              className="mt-1 h-20 w-full rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white"
              value={state.message}
              onChange={(event) => state.setMessage(event.target.value)}
              placeholder="Optional operator note for the generated action"
            />
          </label>

          <div className="mt-3 flex flex-wrap gap-3 text-[11px] text-white/70">
            <label className="inline-flex items-center gap-2">
              <input checked={state.controls} onChange={(event) => state.setControls(event.target.checked)} type="checkbox" />
              controls
            </label>
            <label className="inline-flex items-center gap-2">
              <input checked={state.autoplay} onChange={(event) => state.setAutoplay(event.target.checked)} type="checkbox" />
              autoplay
            </label>
            <label className="inline-flex items-center gap-2">
              <input checked={state.muted} onChange={(event) => state.setMuted(event.target.checked)} type="checkbox" />
              muted
            </label>
            <label className="inline-flex items-center gap-2">
              <input checked={state.loop} onChange={(event) => state.setLoop(event.target.checked)} type="checkbox" />
              loop
            </label>
          </div>

          {state.actionError ? (
            <div className="mt-3 rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
              {state.actionError}
            </div>
          ) : null}

          <div className="mt-3 flex flex-wrap gap-2">
            <button
              data-testid="voice-panel-play"
              className="rounded-md border border-emerald-400/30 bg-emerald-500/10 px-3 py-1.5 text-xs text-emerald-100 hover:bg-emerald-500/20 disabled:opacity-50"
              type="button"
              disabled={!state.canQuery || state.actionMutation.isPending}
              onClick={() => void state.actionMutation.mutateAsync("play")}
            >
              {state.actionMutation.isPending ? "Sending…" : "Play"}
            </button>
            <button
              data-testid="voice-panel-pause"
              className="rounded-md border border-amber-400/30 bg-amber-500/10 px-3 py-1.5 text-xs text-amber-100 hover:bg-amber-500/20 disabled:opacity-50"
              type="button"
              disabled={!state.canQuery || state.actionMutation.isPending}
              onClick={() => void state.actionMutation.mutateAsync("pause")}
            >
              Pause
            </button>
            <button
              data-testid="voice-panel-snapshot"
              className="rounded-md border border-cyan-400/30 bg-cyan-500/10 px-3 py-1.5 text-xs text-cyan-100 hover:bg-cyan-500/20 disabled:opacity-50"
              type="button"
              disabled={!state.canQuery || state.actionMutation.isPending}
              onClick={() => void state.actionMutation.mutateAsync("snapshot")}
            >
              Snapshot
            </button>
          </div>

          {state.lastActionResponse ? (
            <div className="mt-3 rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/70" data-testid="voice-panel-last-action">
              <div>last action: {state.lastActionResponse.action || "unknown"}</div>
              <div>rpc: {state.lastActionResponse.rpc_kind || "unknown"}</div>
              <div>tool call: {state.lastActionResponse.tool_call_id || "n/a"}</div>
              <div>pending client execution: {state.lastActionResponse.pending_client_execution ? "yes" : "no"}</div>
            </div>
          ) : null}
        </section>

        <section className="rounded-md border border-white/10 bg-black/20 p-3" data-testid="voice-stats-section">
          <div className="mb-3 flex flex-wrap items-center justify-between gap-2">
            <div>
              <div className="text-xs font-semibold text-white/80">Voice stats</div>
              <div className="text-[11px] text-white/50">Summaries over durable voice-related client RPC results.</div>
            </div>
            <div className="flex flex-wrap items-center gap-3">
              <label className="inline-flex items-center gap-2 text-[11px] text-white/60">
                <span>max bytes</span>
                <input
                  data-testid="voice-panel-max-bytes"
                  className="w-24 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white"
                  value={state.maxBytesText}
                  onChange={(event) => state.setMaxBytesText(event.target.value)}
                />
              </label>
              <label className="inline-flex items-center gap-2 text-[11px] text-white/60">
                <input
                  checked={state.autoRefresh}
                  onChange={(event) => state.setAutoRefresh(event.target.checked)}
                  type="checkbox"
                />
                auto refresh
              </label>
              <button
                data-testid="voice-panel-refresh"
                className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={!state.canQuery || state.statsQuery.isFetching}
                onClick={() => void state.statsQuery.refetch()}
              >
                {state.statsQuery.isFetching ? "Loading…" : "Refresh"}
              </button>
            </div>
          </div>

          {state.statsQuery.error ? (
            <div className="mb-3 rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
              {String(state.statsQuery.error)}
            </div>
          ) : null}

          <div className="grid gap-2 text-[11px] text-white/70 md:grid-cols-2">
            <div className="rounded-md border border-white/10 bg-black/30 px-2 py-2">
              scanned events: {state.statsQuery.data?.scanned_events ?? 0}
            </div>
            <div className="rounded-md border border-white/10 bg-black/30 px-2 py-2">
              result count: {state.statsQuery.data?.result_count ?? 0}
            </div>
            <div className="rounded-md border border-white/10 bg-black/30 px-2 py-2">
              clients: {state.statsQuery.data?.client_count ?? 0}
            </div>
            <div className="rounded-md border border-white/10 bg-black/30 px-2 py-2">
              counts: play {String(state.counts["media_play"] ?? 0)} · pause {String(state.counts["media_pause"] ?? 0)} · snapshot {String(state.counts["media_snapshot"] ?? 0)}
            </div>
          </div>

          <div className="mt-3 grid gap-3 lg:grid-cols-3">
            <div className="rounded-md border border-white/10 bg-black/30 p-2">
              <div className="mb-1 text-[11px] font-semibold text-white/60">Clients</div>
              {state.clients.length === 0 ? (
                <div className="text-[11px] text-white/50">No voice clients observed yet.</div>
              ) : (
                <pre className="overflow-x-auto whitespace-pre-wrap text-[11px] text-white/70" data-testid="voice-panel-clients">
                  {renderJson(state.clients)}
                </pre>
              )}
            </div>
            <div className="rounded-md border border-white/10 bg-black/30 p-2">
              <div className="mb-1 text-[11px] font-semibold text-white/60">Latest result</div>
              <pre className="overflow-x-auto whitespace-pre-wrap text-[11px] text-white/70" data-testid="voice-panel-latest-result">
                {renderJson(state.statsQuery.data?.latest_result ?? {})}
              </pre>
            </div>
            <div className="rounded-md border border-white/10 bg-black/30 p-2">
              <div className="mb-1 text-[11px] font-semibold text-white/60">Latest snapshot</div>
              <pre className="overflow-x-auto whitespace-pre-wrap text-[11px] text-white/70" data-testid="voice-panel-latest-snapshot">
                {renderJson(state.statsQuery.data?.latest_snapshot ?? {})}
              </pre>
            </div>
          </div>

          <div className="mt-3 rounded-md border border-white/10 bg-black/30 p-2">
            <div className="mb-1 text-[11px] font-semibold text-white/60">Recent results</div>
            <pre className="overflow-x-auto whitespace-pre-wrap text-[11px] text-white/70" data-testid="voice-panel-recent-results">
              {renderJson(state.recentResults)}
            </pre>
          </div>
        </section>
      </div>
    </details>
  );
}
