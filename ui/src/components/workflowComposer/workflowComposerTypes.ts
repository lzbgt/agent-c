import type { Dispatch, SetStateAction } from "react";
import type { WorkflowDetailResp, WorkflowSubmitResp } from "../../api";
import type { ApiAuth } from "../../api/auth";
import type { WorkflowDefaults } from "../../workflowTypes";
import type { GraphBuildResult, GraphState } from "../../workflowGraph";

export type WorkflowComposerProps = {
  baseUrl: string;
  auth?: ApiAuth;
  authKey?: string;
  clientId?: string;
  workflowDefaults?: WorkflowDefaults;
  workflowTargets?: string[];
  workflowBearerEnv?: string;
  onSubmitted?: (workflowId: string) => void;
};

export type TemplateKind = "llm_dag" | "agent_parallel" | "agent_parallel_demo";
export type ComposerMode = "json" | "graph";
export type GraphSetter = Dispatch<SetStateAction<GraphState>>;
export type WaitState = {
  workflowId: string;
  status: string;
  elapsedSec: number;
  active: boolean;
  startedUnixMs: number;
};
export type WaitStatePersisted = {
  workflow_id: string;
  started_unix_ms: number;
  last_status?: string;
  updated_unix_ms?: number;
};
export type WorkflowComposerSubmitResult = WorkflowSubmitResp | WorkflowDetailResp;

export type WorkflowComposerGraphBuild = {
  result: GraphBuildResult | null;
  error: string | null;
};
