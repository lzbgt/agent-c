import React from "react";

import type { ConversationUiActionCardProps } from "./conversationUiActionTypes";
import { runConversationUiActionRpc } from "./conversationRpcExecutor";
import type { ConversationUiActionRpcRequest } from "./conversationRpcTypes";
import { globalAutoRunOnceMap } from "./utils";

const RPC_SIDE_EFFECT_KINDS = new Set([
  "dom_click",
  "dom_set_value",
  "dom_apply",
  "entity_apply",
  "media_play",
  "media_pause",
  "media_observe",
  "navigate",
  "open_url",
  "page_eval",
]);

function deriveRpcRequest(data: any, sessionId?: string, allowClientRpcs = false, allowClientEffects = false): ConversationUiActionRpcRequest {
  const action = data?.action ?? {};
  const atype = String(action?.type ?? "");
  const title = String(action?.title ?? (atype ? `ui_action: ${atype}` : "ui_action"));
  const toolCallId = String(data?.tool_call_id ?? "");
  const rpcId = String(action?.rpc_id ?? action?.probe_id ?? toolCallId ?? "").trim();
  const rpc = action?.rpc ?? action?.probe ?? {};
  const rpcKind = String(rpc?.kind ?? "").trim();
  const rpcArgs = typeof rpc?.args === "object" && rpc?.args ? rpc.args : rpc;
  const sideEffectsRequested =
    rpc?.side_effects === true || action?.side_effects === true || RPC_SIDE_EFFECT_KINDS.has(rpcKind);
  const canRun = !!rpcId && typeof sessionId === "string" && sessionId.trim().length > 0;
  const canRunAuto = !!allowClientRpcs && (!sideEffectsRequested || !!allowClientEffects);
  const autoRunRequested =
    typeof action?.auto_run === "boolean" ? action.auto_run : typeof action?.auto === "boolean" ? action.auto : true;
  return {
    atype,
    title,
    toolCallId,
    rpcId,
    rpcKind,
    rpcArgs,
    sideEffectsRequested,
    autoRunRequested,
    canRun,
    canRunAuto,
    autoRun: canRunAuto && autoRunRequested,
  };
}

export function useConversationUiActionRpcState({
  baseUrl,
  yolo,
  sessionId,
  daemonAuth,
  allowClientRpcs,
  allowClientEffects,
  allowUnsafePageEval,
  disableAutoClientRpcs,
  data,
  idx,
  postClientEvent,
  runtime,
  sceneEntities,
  onSceneApply,
}: ConversationUiActionCardProps) {
  const request = React.useMemo(
    () => deriveRpcRequest(data, sessionId, allowClientRpcs, allowClientEffects),
    [allowClientEffects, allowClientRpcs, data, sessionId],
  );

  const runRpc = React.useCallback(async () => {
    await runConversationUiActionRpc({
      baseUrl,
      yolo,
      sessionId,
      daemonAuth,
      allowClientRpcs,
      allowClientEffects,
      allowUnsafePageEval,
      atype: request.atype,
      toolCallId: request.toolCallId,
      rpcId: request.rpcId,
      rpcKind: request.rpcKind,
      rpcArgs: request.rpcArgs,
      sideEffectsRequested: request.sideEffectsRequested,
      postClientEvent,
      runtime,
      sceneEntities,
      onSceneApply,
    });
  }, [
    allowClientEffects,
    allowClientRpcs,
    allowUnsafePageEval,
    baseUrl,
    daemonAuth,
    onSceneApply,
    postClientEvent,
    request.atype,
    request.rpcArgs,
    request.rpcId,
    request.rpcKind,
    request.sideEffectsRequested,
    request.toolCallId,
    runtime,
    sceneEntities,
    sessionId,
    yolo,
  ]);

  const ackKey = React.useMemo(() => `rpc:${request.toolCallId || request.rpcId || idx}`, [idx, request.rpcId, request.toolCallId]);
  const sidKey = typeof sessionId === "string" ? sessionId.trim() : "";
  const globalKey = React.useMemo(() => `${sidKey || "no_session"}::${ackKey}`, [ackKey, sidKey]);
  const globalOnce = globalAutoRunOnceMap();
  const alreadyRan = !!runtime.probeRanRef.current[ackKey] || !!globalOnce[globalKey];
  const runDisabled = !request.canRun || !allowClientRpcs || (request.sideEffectsRequested && !allowClientEffects);
  const runDisabledReason = !allowClientRpcs
    ? "Enable “Allow agent-requested client RPCs” in settings to run"
    : request.sideEffectsRequested && !allowClientEffects
      ? "Enable “Allow agent-requested client RPCs with side effects” in settings to run"
      : "";

  React.useEffect(() => {
    if (disableAutoClientRpcs || !request.autoRun || request.rpcKind === "entity_apply" || alreadyRan || !request.canRun) {
      return;
    }
    runtime.markSeenWithLimit(runtime.probeRanRef.current, ackKey, runtime.localUiActionLimit);
    globalOnce[globalKey] = true;
    runtime.pendingAutoRunsRef.current[globalKey] = () => {
      void runRpc().catch(() => {});
    };
    return () => {
      const pending = runtime.pendingAutoRunsRef.current;
      if (pending[globalKey]) delete pending[globalKey];
    };
  }, [
    ackKey,
    alreadyRan,
    disableAutoClientRpcs,
    globalKey,
    globalOnce,
    request.autoRun,
    request.canRun,
    request.rpcKind,
    runRpc,
    runtime,
  ]);

  const onRun = React.useCallback(() => {
    runtime.markSeenWithLimit(runtime.probeRanRef.current, ackKey, runtime.localUiActionLimit);
    void runRpc().catch(() => {});
  }, [ackKey, runRpc, runtime]);

  return {
    ...request,
    ackKey,
    runDisabled,
    runDisabledReason,
    onRun,
  };
}
