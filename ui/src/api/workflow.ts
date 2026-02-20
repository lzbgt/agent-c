import { daemonHeaders, type ApiAuth } from "./auth";
import { addQueryParam } from "./query";
import {
  WorkflowDetailRespSchema,
  type WorkflowDetailResp,
  WorkflowListRespSchema,
  type WorkflowListResp,
  WorkflowSubmitRespSchema,
  type WorkflowSubmitResp,
} from "./schemas/workflow";

export type WorkflowListParams = {
  status?: string;
  limit?: number;
};

export async function apiListWorkflows(
  base: string,
  params: WorkflowListParams,
  auth?: ApiAuth,
): Promise<WorkflowListResp> {
  const qs = new URLSearchParams();
  addQueryParam(qs, "status", params.status);
  addQueryParam(qs, "limit", params.limit);
  const q = qs.toString();
  const r = await fetch(`${base}/api/v1/workflows${q ? `?${q}` : ""}`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return WorkflowListRespSchema.parse(j);
}

export type WorkflowGetParams = {
  workflowId: string;
  includeTasks?: boolean;
  includeResults?: boolean;
  includeSpec?: boolean;
};

export async function apiGetWorkflow(
  base: string,
  params: WorkflowGetParams,
  auth?: ApiAuth,
): Promise<WorkflowDetailResp> {
  const qs = new URLSearchParams();
  addQueryParam(qs, "workflow_id", params.workflowId);
  addQueryParam(qs, "include_tasks", params.includeTasks);
  addQueryParam(qs, "include_results", params.includeResults);
  addQueryParam(qs, "include_spec", params.includeSpec);
  const q = qs.toString();
  const r = await fetch(`${base}/api/v1/workflow${q ? `?${q}` : ""}`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return WorkflowDetailRespSchema.parse(j);
}

export async function apiSubmitWorkflow(
  base: string,
  payload: Record<string, any>,
  auth?: ApiAuth,
): Promise<WorkflowSubmitResp> {
  const r = await fetch(`${base}/api/v1/workflow/submit`, {
    method: "POST",
    headers: daemonHeaders(auth, { "Content-Type": "application/json" }),
    body: JSON.stringify(payload),
  });
  const j = await r.json();
  return WorkflowSubmitRespSchema.parse(j);
}
