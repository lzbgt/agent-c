import type { ApiAuth } from "../../api";
import type { SceneEntity } from "../SceneView";
import type { ConversationRpcRuntime } from "./conversationViewTypes";

export type ConversationUiActionRpcRequest = {
  atype: string;
  title: string;
  toolCallId: string;
  rpcId: string;
  rpcKind: string;
  rpcArgs: any;
  sideEffectsRequested: boolean;
  autoRunRequested: boolean;
  canRun: boolean;
  canRunAuto: boolean;
  autoRun: boolean;
};

export type ConversationUiActionRpcExecutorInput = {
  baseUrl: string;
  yolo: boolean;
  sessionId?: string;
  daemonAuth?: ApiAuth;
  allowClientRpcs: boolean;
  allowClientEffects: boolean;
  allowUnsafePageEval: boolean;
  atype: string;
  toolCallId: string;
  rpcId: string;
  rpcKind: string;
  rpcArgs: any;
  sideEffectsRequested: boolean;
  postClientEvent: (type: string, payload: any) => Promise<void>;
  runtime: ConversationRpcRuntime;
  sceneEntities?: SceneEntity[];
  onSceneApply?: (ops: unknown[]) => unknown;
};
