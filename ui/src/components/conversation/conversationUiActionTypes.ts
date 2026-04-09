import type { ApiAuth } from "../../api";
import type { SceneEntity } from "../SceneView";
import type { ConversationRpcRuntime } from "./conversationViewTypes";

export type ConversationUiActionCardProps = {
  baseUrl: string;
  yolo: boolean;
  sessionId?: string;
  daemonAuth?: ApiAuth;
  allowClientRpcs: boolean;
  allowClientEffects: boolean;
  allowUnsafePageEval: boolean;
  disableAutoClientRpcs?: boolean;
  data: any;
  idx: number;
  ackedKeys: Record<string, number>;
  ackError: string | null;
  setAckError: (msg: string | null) => void;
  markAckedKey: (key: string) => void;
  postClientEvent: (type: string, payload: any) => Promise<void>;
  runtime: ConversationRpcRuntime;
  sceneEntities?: SceneEntity[];
  onSceneApply?: (ops: unknown[]) => unknown;
};
