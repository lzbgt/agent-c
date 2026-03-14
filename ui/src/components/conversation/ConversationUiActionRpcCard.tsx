import React from "react";

import ConversationCard from "../ConversationCard";
import type { ConversationUiActionCardProps } from "./conversationUiActionTypes";
import { useConversationUiActionRpcState } from "./useConversationUiActionRpcState";

const Card = ConversationCard;

export default function ConversationUiActionRpcCard(props: ConversationUiActionCardProps) {
  const {
    atype,
    title,
    toolCallId,
    rpcId,
    rpcKind,
    rpcArgs,
    sideEffectsRequested,
    autoRunRequested,
    canRun,
    runDisabled,
    runDisabledReason,
    onRun,
  } = useConversationUiActionRpcState(props);

  if (!(atype === "client_rpc" || atype === "collab_rpc" || atype === "client_probe")) {
    return null;
  }

  return (
    <Card key={`ua-${props.idx}`} title={`UI action: ${title}`}>
      <div className="rounded-md border border-white/10 bg-black/20 px-3 py-2 text-sm text-white/80">
        Client RPC requested: <code>{rpcKind || "(missing kind)"}</code>
        {rpcId ? (
          <span className="ml-2 text-[11px] text-white/50">
            rpc_id=<code>{rpcId}</code>
          </span>
        ) : null}
      </div>
      <details className="mt-2 rounded-md border border-white/10 bg-black/20 px-3 py-2">
        <summary className="cursor-pointer select-none text-[11px] font-semibold text-white/70">Request payload</summary>
        <pre className="mt-2 overflow-auto whitespace-pre-wrap font-mono text-[11px] leading-relaxed text-white/90">
          {JSON.stringify(
            {
              type: atype,
              tool_call_id: toolCallId || undefined,
              rpc_id: rpcId,
              rpc: { kind: rpcKind, args: rpcArgs },
              side_effects: sideEffectsRequested,
              auto_run: autoRunRequested,
            },
            null,
            2,
          )}
        </pre>
      </details>
      <div className="mt-2 flex flex-wrap items-center gap-2">
        <button
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
          type="button"
          disabled={runDisabled}
          title={runDisabledReason}
          onClick={onRun}
        >
          Run RPC
        </button>
        {!props.allowClientRpcs ? (
          <div className="text-[11px] text-white/40">Disabled by settings</div>
        ) : sideEffectsRequested && !props.allowClientEffects ? (
          <div className="text-[11px] text-white/40">Side effects disabled</div>
        ) : !canRun ? (
          <div className="text-[11px] text-white/40">Missing session or rpc_id</div>
        ) : null}
      </div>
    </Card>
  );
}
