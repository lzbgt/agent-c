import React from "react";
import type { ApiAuth } from "../../api";
import { fmtTs } from "./useBrokerPanelState";
import useBrokerAudioState from "./useBrokerAudioState";

type BrokerAudioSectionProps = {
  base: string;
  auth: ApiAuth;
  canQuery: boolean;
  agentId: string;
  defaultDeploymentId?: string;
};

export default function BrokerAudioSection(props: BrokerAudioSectionProps) {
  const state = useBrokerAudioState({
    base: props.base,
    auth: props.auth,
    canQuery: props.canQuery,
    agentId: props.agentId,
    defaultDeploymentId: props.defaultDeploymentId,
  });

  return (
    <section className="rounded-md border border-white/10 bg-black/20 p-3" data-testid="broker-audio-section">
      <div className="mb-3 flex flex-wrap items-center justify-between gap-2">
        <div>
          <div className="text-xs font-semibold text-white/80">Voice signaling sessions</div>
          <div className="text-[11px] text-white/50">
            Explicit broker-side lifecycle and signal inspection for WebRTC audio sessions.
          </div>
        </div>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
          type="button"
          disabled={!props.canQuery || state.sessionsQuery.isFetching}
          onClick={() => void state.sessionsQuery.refetch()}
        >
          {state.sessionsQuery.isFetching ? "Loading…" : "Refresh"}
        </button>
      </div>

      {!props.agentId ? (
        <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-3 py-2 text-[11px] text-amber-100">
          Select an agent in the Broker panel first.
        </div>
      ) : null}

      <div className="grid gap-4 lg:grid-cols-[minmax(0,360px)_minmax(0,1fr)]">
        <div className="grid gap-3">
          <div className="rounded-md border border-white/10 bg-black/30 p-3">
            <div className="mb-2 text-[11px] font-semibold text-white/60">Create session</div>
            <label className="mb-2 block text-[11px] text-white/60">
              Deployment
              <input
                data-testid="broker-audio-deployment"
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white"
                value={state.deploymentId}
                onChange={(ev) => state.setDeploymentId(ev.target.value)}
                placeholder={props.defaultDeploymentId || "default"}
              />
            </label>
            <label className="mb-2 block text-[11px] text-white/60">
              Metadata JSON
              <textarea
                data-testid="broker-audio-metadata"
                className="mt-1 h-24 w-full rounded-md border border-white/10 bg-black/30 px-2 py-1 font-mono text-[11px] text-white"
                value={state.metadataText}
                onChange={(ev) => state.setMetadataText(ev.target.value)}
              />
            </label>
            {state.createError ? (
              <div className="mb-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
                {state.createError}
              </div>
            ) : null}
            <button
              data-testid="broker-audio-create"
              className="rounded-md border border-emerald-400/30 bg-emerald-500/10 px-3 py-1.5 text-xs text-emerald-100 hover:bg-emerald-500/20 disabled:opacity-50"
              type="button"
              disabled={!props.canQuery || !props.agentId || state.createMutation.isPending}
              onClick={() => void state.createMutation.mutateAsync()}
            >
              {state.createMutation.isPending ? "Creating…" : "Create voice session"}
            </button>
          </div>

          <div className="rounded-md border border-white/10 bg-black/30 p-3">
            <div className="mb-2 text-[11px] font-semibold text-white/60">Live sessions</div>
            {state.sessions.length === 0 ? (
              <div className="text-[11px] text-white/50">No live audio sessions for the current filter.</div>
            ) : (
              <div className="grid gap-2">
                {state.sessions.map((session) => {
                  const active = session.session_id === state.selectedSessionId;
                  return (
                    <button
                      key={session.session_id}
                      data-testid={`broker-audio-session-${session.session_id}`}
                      className={`rounded-md border px-2 py-2 text-left ${
                        active
                          ? "border-indigo-400/40 bg-indigo-500/10"
                          : "border-white/10 bg-black/20 hover:bg-black/30"
                      }`}
                      type="button"
                      onClick={() => state.setSelectedSessionId(session.session_id)}
                    >
                      <div className="flex flex-wrap items-center justify-between gap-2">
                        <div className="text-xs text-white/90">{session.session_id}</div>
                        <div className="text-[11px] text-white/50">{session.mode || "webrtc"}</div>
                      </div>
                      <div className="mt-1 text-[11px] text-white/60">
                        {session.agent_id}
                        {session.deployment_id ? ` · ${session.deployment_id}` : ""}
                      </div>
                      <div className="mt-1 text-[11px] text-white/50">
                        signals {session.signal_count} · subscribers {session.subscriber_count}
                      </div>
                    </button>
                  );
                })}
              </div>
            )}
          </div>
        </div>

        <div className="grid gap-3">
          <div className="rounded-md border border-white/10 bg-black/30 p-3">
            <div className="mb-2 flex flex-wrap items-center justify-between gap-2">
              <div className="text-[11px] font-semibold text-white/60">Selected session</div>
              <div className="text-[11px] text-white/50">
                {state.streamConnected ? "stream connected" : state.selectedSessionId ? "stream idle" : "no session selected"}
              </div>
            </div>
            {state.selectedSession ? (
              <div className="grid gap-1 text-[11px] text-white/70" data-testid="broker-audio-selected-session">
                <div>session: {state.selectedSession.session_id}</div>
                <div>agent: {state.selectedSession.agent_id}</div>
                <div>deployment: {state.selectedSession.deployment_id || "default"}</div>
                <div>expires: {fmtTs(state.selectedSession.expires_unix_ms)}</div>
                <div>signals: {state.selectedSession.signal_count}</div>
                <div>subscribers: {state.selectedSession.subscriber_count}</div>
                <div>
                  last signal: {state.selectedSession.last_signal_type || "none"}
                  {state.selectedSession.last_signal_from ? ` from ${state.selectedSession.last_signal_from}` : ""}
                </div>
              </div>
            ) : (
              <div className="text-[11px] text-white/50">Select a live session to inspect status and signals.</div>
            )}
            {state.streamError ? (
              <div className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
                {state.streamError}
              </div>
            ) : null}
            {state.deleteError ? (
              <div className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
                {state.deleteError}
              </div>
            ) : null}
            <div className="mt-3 flex flex-wrap gap-2">
              <button
                data-testid="broker-audio-delete"
                className="rounded-md border border-rose-400/30 bg-rose-500/10 px-3 py-1.5 text-xs text-rose-100 hover:bg-rose-500/20 disabled:opacity-50"
                type="button"
                disabled={!state.selectedSessionId || state.deleteMutation.isPending}
                onClick={() => void state.deleteMutation.mutateAsync()}
              >
                {state.deleteMutation.isPending ? "Deleting…" : "Delete session"}
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-1.5 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={!state.selectedSessionId || state.sessionQuery.isFetching}
                onClick={() => void state.sessionQuery.refetch()}
              >
                Reload status
              </button>
            </div>
          </div>

          <div className="rounded-md border border-white/10 bg-black/30 p-3">
            <div className="mb-2 text-[11px] font-semibold text-white/60">Send signal</div>
            <div className="grid gap-2 md:grid-cols-[180px_minmax(0,1fr)]">
              <label className="text-[11px] text-white/60">
                Type
                <select
                  data-testid="broker-audio-signal-type"
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white"
                  value={state.signalType}
                  onChange={(ev) => state.setSignalType(ev.target.value)}
                >
                  <option value="control">control</option>
                  <option value="offer">offer</option>
                  <option value="answer">answer</option>
                  <option value="candidate">candidate</option>
                  <option value="bye">bye</option>
                </select>
              </label>
              <label className="text-[11px] text-white/60">
                Payload JSON
                <textarea
                  data-testid="broker-audio-signal-payload"
                  className="mt-1 h-24 w-full rounded-md border border-white/10 bg-black/30 px-2 py-1 font-mono text-[11px] text-white"
                  value={state.signalPayloadText}
                  onChange={(ev) => state.setSignalPayloadText(ev.target.value)}
                />
              </label>
            </div>
            {state.sendError ? (
              <div className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
                {state.sendError}
              </div>
            ) : null}
            <button
              data-testid="broker-audio-send"
              className="mt-2 rounded-md border border-cyan-400/30 bg-cyan-500/10 px-3 py-1.5 text-xs text-cyan-100 hover:bg-cyan-500/20 disabled:opacity-50"
              type="button"
              disabled={!state.selectedSessionId || state.sendMutation.isPending}
              onClick={() => void state.sendMutation.mutateAsync()}
            >
              {state.sendMutation.isPending ? "Sending…" : "Send signal"}
            </button>
          </div>

          <div className="rounded-md border border-white/10 bg-black/30 p-3">
            <div className="mb-2 flex items-center justify-between gap-2">
              <div className="text-[11px] font-semibold text-white/60">Signal stream</div>
              <div className="text-[11px] text-white/50">{state.signalEvents.length} events buffered</div>
            </div>
            {state.signalEvents.length === 0 ? (
              <div className="text-[11px] text-white/50">No signal events received yet for the selected session.</div>
            ) : (
              <div className="grid gap-2" data-testid="broker-audio-signal-events">
                {state.signalEvents.map((ev, idx) => (
                  <div key={`${ev.ts_unix_ms}:${ev.type}:${idx}`} className="rounded-md border border-white/10 bg-black/20 px-2 py-1">
                    <div className="flex flex-wrap items-center justify-between gap-2 text-[11px] text-white/80">
                      <span>{ev.type}</span>
                      <span>{ev.from || "unknown"} · {fmtTs(ev.ts_unix_ms)}</span>
                    </div>
                    <pre className="mt-1 overflow-x-auto whitespace-pre-wrap text-[11px] text-white/60">
                      {ev.payload ? JSON.stringify(ev.payload, null, 2) : "{}"}
                    </pre>
                  </div>
                ))}
              </div>
            )}
          </div>
        </div>
      </div>
    </section>
  );
}
