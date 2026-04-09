import React from "react";
import ConversationCard from "../ConversationCard";
import ConversationUiActionAckCard from "./ConversationUiActionAckCard";
import { parseConversationUiAction } from "./conversationData";
import ConversationUiActionRpcCard from "./ConversationUiActionRpcCard";
import type { ConversationUiActionCardProps } from "./conversationUiActionTypes";

const Card = ConversationCard;
export type { ConversationUiActionCardProps } from "./conversationUiActionTypes";

export default function ConversationUiActionCard({
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
  ackedKeys,
  ackError,
  setAckError,
  markAckedKey,
  postClientEvent,
  runtime,
  sceneEntities,
  onSceneApply,
}: ConversationUiActionCardProps) {
  const parsed = parseConversationUiAction(data);
  const action = parsed.action;
  const atype = parsed.atype;
  const title = parsed.title;
  if (atype === "client_rpc" || atype === "collab_rpc" || atype === "client_probe") {
    return (
      <ConversationUiActionRpcCard
        baseUrl={baseUrl}
        yolo={yolo}
        sessionId={sessionId}
        daemonAuth={daemonAuth}
        allowClientRpcs={allowClientRpcs}
        allowClientEffects={allowClientEffects}
        allowUnsafePageEval={allowUnsafePageEval}
        disableAutoClientRpcs={disableAutoClientRpcs}
        data={data}
        idx={idx}
        ackedKeys={ackedKeys}
        ackError={ackError}
        setAckError={setAckError}
        markAckedKey={markAckedKey}
        postClientEvent={postClientEvent}
        runtime={runtime}
        sceneEntities={sceneEntities}
        onSceneApply={onSceneApply}
      />
    );
  }

  if (atype === "request_client_state" || atype === "request_state" || atype === "notify") {
    return (
      <ConversationUiActionAckCard
        baseUrl={baseUrl}
        yolo={yolo}
        sessionId={sessionId}
        daemonAuth={daemonAuth}
        allowClientRpcs={allowClientRpcs}
        allowClientEffects={allowClientEffects}
        allowUnsafePageEval={allowUnsafePageEval}
        disableAutoClientRpcs={disableAutoClientRpcs}
        data={data}
        idx={idx}
        ackedKeys={ackedKeys}
        ackError={ackError}
        setAckError={setAckError}
        markAckedKey={markAckedKey}
        postClientEvent={postClientEvent}
        runtime={runtime}
        sceneEntities={sceneEntities}
        onSceneApply={onSceneApply}
      />
    );
  }

  return (
    <Card key={`ua-${idx}`} title={`UI action: ${title}`}>
      <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
        {JSON.stringify(action, null, 2)}
      </pre>
    </Card>
  );
}
