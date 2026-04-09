import React from "react";

import { deriveConversationRpcRequest } from "./conversationData";
import type { ConversationUiActionCardProps } from "./conversationUiActionTypes";
import { runConversationUiActionRpc } from "./conversationRpcExecutor";
import { globalAutoRunOnceMap } from "./utils";

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
    () => deriveConversationRpcRequest(data, sessionId, allowClientRpcs, allowClientEffects),
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
