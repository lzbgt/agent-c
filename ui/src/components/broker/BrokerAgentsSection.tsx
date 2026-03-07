import React from "react";
import type { BrokerAgentInfo } from "../../api";

type BrokerAgentsSectionProps = {
  canQuery: boolean;
  isFetching: boolean;
  error: unknown;
  agents: BrokerAgentInfo[];
  agentId: string;
  onRefresh: () => void;
  onSelectAgent: (id: string) => void;
};

export default function BrokerAgentsSection(props: BrokerAgentsSectionProps) {
  const { canQuery, isFetching, error, agents, agentId, onRefresh, onSelectAgent } = props;

  return (
    <section className="rounded-md border border-white/10 bg-black/20 p-3">
      <div className="mb-2 flex items-center justify-between gap-2">
        <div className="text-xs font-semibold text-white/80">Agent list</div>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
          type="button"
          disabled={!canQuery || isFetching}
          onClick={onRefresh}
        >
          {isFetching ? "Loading…" : "Refresh"}
        </button>
      </div>

      {error ? (
        <div className="mb-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
          {String(error)}
        </div>
      ) : null}

      {agents.length === 0 ? (
        <div className="text-[11px] text-white/50">No agents returned.</div>
      ) : (
        <div className="grid gap-2">
          {agents.map((agent) => {
            const id = String(agent?.agent_id || "");
            const connected = agent?.connected === true;
            const selected = id && id === agentId;
            return (
              <div
                key={id}
                className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-white/5 bg-black/30 px-2 py-1"
              >
                <div className="flex flex-col">
                  <div className="text-xs text-white/90">{id}</div>
                  <div className="text-[11px] text-white/50">
                    {connected ? "connected" : "disconnected"}
                    {agent?.owner_sub ? ` · owner ${String(agent.owner_sub)}` : ""}
                  </div>
                </div>
                <button
                  className={
                    selected
                      ? "rounded-md border border-emerald-400/40 bg-emerald-500/10 px-2 py-1 text-[11px] text-emerald-100"
                      : "rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                  }
                  type="button"
                  onClick={() => onSelectAgent(id)}
                >
                  {selected ? "Selected" : "Use"}
                </button>
              </div>
            );
          })}
        </div>
      )}
    </section>
  );
}
