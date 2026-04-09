import { daemonFetchInit, type ApiAuth } from "./auth";
import { addQueryParam } from "./query";
import type { WorkflowSubmitRequest } from "../workflowTypes";
import {
  WorkflowDetailRespSchema,
  type WorkflowDetailResp,
  WorkflowListRespSchema,
  type WorkflowListResp,
  WorkflowCancelRespSchema,
  type WorkflowCancelResp,
  WorkflowSubmitRespSchema,
  type WorkflowSubmitResp,
} from "./schemas/workflow";

export type WorkflowListParams = {
  status?: string;
  limit?: number;
  query?: string;
};

export async function apiListWorkflows(
  base: string,
  params: WorkflowListParams,
  auth?: ApiAuth,
): Promise<WorkflowListResp> {
  const qs = new URLSearchParams();
  addQueryParam(qs, "status", params.status);
  addQueryParam(qs, "limit", params.limit);
  addQueryParam(qs, "q", params.query);
  const q = qs.toString();
  const r = await fetch(`${base}/api/v1/workflows${q ? `?${q}` : ""}`, daemonFetchInit(auth));
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
  const r = await fetch(`${base}/api/v1/workflow${q ? `?${q}` : ""}`, daemonFetchInit(auth));
  const j = await r.json();
  return WorkflowDetailRespSchema.parse(j);
}

export async function apiSubmitWorkflow(
  base: string,
  payload: WorkflowSubmitRequest,
  auth?: ApiAuth,
): Promise<WorkflowSubmitResp> {
  const r = await fetch(
    `${base}/api/v1/workflow/submit`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(payload) }, { "Content-Type": "application/json" }),
  );
  const j = await r.json();
  return WorkflowSubmitRespSchema.parse(j);
}

export async function apiCancelWorkflow(
  base: string,
  workflowId: string,
  auth?: ApiAuth,
): Promise<WorkflowCancelResp> {
  const r = await fetch(
    `${base}/api/v1/workflow/cancel`,
    daemonFetchInit(
      auth,
      { method: "POST", body: JSON.stringify({ workflow_id: workflowId }) },
      { "Content-Type": "application/json" },
    ),
  );
  const j = await r.json();
  return WorkflowCancelRespSchema.parse(j);
}
