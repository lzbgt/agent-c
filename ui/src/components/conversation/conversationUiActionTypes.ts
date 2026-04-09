import type { ApiAuth } from "../../api";
import type { SceneEntity } from "../SceneView";
import type { SceneEntityMutationOp } from "../scene/sceneViewTypes";
import type { ConversationRpcRuntime } from "./conversationViewTypes";
import type { UnknownRecord } from "./utils";

export type ConversationUiActionCardProps = {
  baseUrl: string;
  yolo: boolean;
  sessionId?: string;
  daemonAuth?: ApiAuth;
  allowClientRpcs: boolean;
  allowClientEffects: boolean;
  allowUnsafePageEval: boolean;
  disableAutoClientRpcs?: boolean;
  data: UnknownRecord;
  idx: number;
  ackedKeys: Record<string, number>;
  ackError: string | null;
  setAckError: (msg: string | null) => void;
  markAckedKey: (key: string) => void;
  postClientEvent: (type: string, payload: unknown) => Promise<void>;
  runtime: ConversationRpcRuntime;
  sceneEntities?: SceneEntity[];
  onSceneApply?: (ops: SceneEntityMutationOp[]) => unknown;
};
