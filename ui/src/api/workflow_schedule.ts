import { daemonHeaders, type ApiAuth } from "./auth";
import { addQueryParam } from "./query";
import {
  WorkflowScheduleCreateRespSchema,
  type WorkflowScheduleCreateResp,
  WorkflowScheduleListRespSchema,
  type WorkflowScheduleListResp,
  WorkflowScheduleGetRespSchema,
  type WorkflowScheduleGetResp,
  WorkflowScheduleUpdateRespSchema,
  type WorkflowScheduleUpdateResp,
  WorkflowScheduleRunsRespSchema,
  type WorkflowScheduleRunsResp,
} from "./schemas/workflow_schedule";

export type WorkflowScheduleListParams = {
  status?: string;
  limit?: number;
  offset?: number;
};

export async function apiListWorkflowSchedules(
  base: string,
  params: WorkflowScheduleListParams,
  auth?: ApiAuth,
): Promise<WorkflowScheduleListResp> {
  const qs = new URLSearchParams();
  addQueryParam(qs, "status", params.status);
  addQueryParam(qs, "limit", params.limit);
  addQueryParam(qs, "offset", params.offset);
  const q = qs.toString();
  const r = await fetch(`${base}/api/v1/workflow_schedules${q ? `?${q}` : ""}`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return WorkflowScheduleListRespSchema.parse(j);
}

export async function apiGetWorkflowSchedule(
  base: string,
  scheduleId: string,
  auth?: ApiAuth,
): Promise<WorkflowScheduleGetResp> {
  const qs = new URLSearchParams();
  addQueryParam(qs, "schedule_id", scheduleId);
  const q = qs.toString();
  const r = await fetch(`${base}/api/v1/workflow_schedule${q ? `?${q}` : ""}`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return WorkflowScheduleGetRespSchema.parse(j);
}

export async function apiCreateWorkflowSchedule(
  base: string,
  payload: Record<string, any>,
  auth?: ApiAuth,
): Promise<WorkflowScheduleCreateResp> {
  const r = await fetch(`${base}/api/v1/workflow_schedules`, {
    method: "POST",
    headers: daemonHeaders(auth, { "Content-Type": "application/json" }),
    body: JSON.stringify(payload),
  });
  const j = await r.json();
  return WorkflowScheduleCreateRespSchema.parse(j);
}

export async function apiDeleteWorkflowSchedule(
  base: string,
  scheduleId: string,
  auth?: ApiAuth,
): Promise<WorkflowScheduleUpdateResp> {
  const qs = new URLSearchParams();
  addQueryParam(qs, "schedule_id", scheduleId);
  const q = qs.toString();
  const r = await fetch(`${base}/api/v1/workflow_schedule${q ? `?${q}` : ""}`, {
    method: "DELETE",
    headers: daemonHeaders(auth),
  });
  const j = await r.json();
  return WorkflowScheduleUpdateRespSchema.parse(j);
}

export async function apiPauseWorkflowSchedule(
  base: string,
  scheduleId: string,
  auth?: ApiAuth,
): Promise<WorkflowScheduleUpdateResp> {
  const r = await fetch(`${base}/api/v1/workflow_schedule/pause`, {
    method: "POST",
    headers: daemonHeaders(auth, { "Content-Type": "application/json" }),
    body: JSON.stringify({ schedule_id: scheduleId }),
  });
  const j = await r.json();
  return WorkflowScheduleUpdateRespSchema.parse(j);
}

export async function apiResumeWorkflowSchedule(
  base: string,
  scheduleId: string,
  auth?: ApiAuth,
): Promise<WorkflowScheduleUpdateResp> {
  const r = await fetch(`${base}/api/v1/workflow_schedule/resume`, {
    method: "POST",
    headers: daemonHeaders(auth, { "Content-Type": "application/json" }),
    body: JSON.stringify({ schedule_id: scheduleId }),
  });
  const j = await r.json();
  return WorkflowScheduleUpdateRespSchema.parse(j);
}

export type WorkflowScheduleRunListParams = {
  scheduleId: string;
  limit?: number;
  offset?: number;
};

export async function apiListWorkflowScheduleRuns(
  base: string,
  params: WorkflowScheduleRunListParams,
  auth?: ApiAuth,
): Promise<WorkflowScheduleRunsResp> {
  const qs = new URLSearchParams();
  addQueryParam(qs, "schedule_id", params.scheduleId);
  addQueryParam(qs, "limit", params.limit);
  addQueryParam(qs, "offset", params.offset);
  const q = qs.toString();
  const r = await fetch(`${base}/api/v1/workflow_schedule/runs${q ? `?${q}` : ""}`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return WorkflowScheduleRunsRespSchema.parse(j);
}
