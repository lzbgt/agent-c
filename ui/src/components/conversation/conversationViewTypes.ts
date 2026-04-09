import React from "react";

import type { AgentEvent, ApiAuth } from "../../api";
import type { SceneEntity } from "../SceneView";

export type RpcCleanupEntry = {
  cleanups: Array<() => void>;
  kind: string;
  createdMs: number;
  lastActiveMs: number;
};

export type ConversationRpcRuntime = {
  pendingAutoRunsRef: React.MutableRefObject<Record<string, () => void>>;
  probeRanRef: React.MutableRefObject<Record<string, number>>;
  rpcCleanupRef: React.MutableRefObject<Record<string, RpcCleanupEntry>>;
  artifactBlobUrlsRef: React.MutableRefObject<string[]>;
  cleanupRpcEntry: (id: string) => boolean;
  markSeenWithLimit: (store: Record<string, number>, key: string, limit: number) => void;
  localUiActionLimit: number;
};

export type ConversationViewProps = {
  baseUrl: string;
  yolo: boolean;
  sessionId?: string;
  client?: { id?: string; kind?: string; instance_id?: string };
  daemonAuth?: ApiAuth;
  prompt: string;
  events: AgentEvent[];
  showDebugEvents?: boolean;
  allowAutoplay: boolean;
  allowClientRpcs: boolean;
  allowClientEffects: boolean;
  allowUnsafePageEval: boolean;
  reverseOrder?: boolean;
  disableAutoClientRpcs?: boolean;
  sceneEntities?: SceneEntity[];
  onSceneApply?: (ops: unknown[]) => unknown;
};

export type ConversationToolCallSummaryById = Record<string, { cmd?: string; argv?: string }>;
