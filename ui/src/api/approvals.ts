import { daemonFetchInit, type ApiAuth } from "./auth";
import {
  ApprovalDecisionRespSchema,
  type ApprovalDecisionResp,
  ApprovalDetailSchema,
  type ApprovalDetailResp,
  ApprovalsListSchema,
  type ApprovalsListResp,
} from "./schemas/approvals";

export type ApprovalsListQuery = {
  status?: string;
  teamId?: string;
  traceId?: string;
  jobId?: string;
  toolName?: string;
  runId?: string | number;
  limit?: number;
};

export type ApprovalDecisionReq = {
  memberId: string;
  memberRole?: string;
  decision: "approve" | "deny";
  note?: string;
};

async function parseJsonOrThrow(r: Response): Promise<any> {
  const text = await r.text();
  try {
    return JSON.parse(text);
  } catch {
    const msg = text ? `HTTP ${r.status}: ${text}` : `HTTP ${r.status}`;
    throw new Error(msg);
  }
}

export async function apiListApprovals(
  base: string,
  query: ApprovalsListQuery,
  auth?: ApiAuth,
): Promise<ApprovalsListResp> {
  const qs = new URLSearchParams();
  if (query.status) qs.set("status", query.status);
  if (query.teamId) qs.set("team_id", query.teamId);
  if (query.traceId) qs.set("trace_id", query.traceId);
  if (query.jobId) qs.set("job_id", query.jobId);
  if (query.toolName) qs.set("tool_name", query.toolName);
  if (query.runId !== undefined && query.runId !== null && `${query.runId}`.trim()) {
    qs.set("run_id", `${query.runId}`.trim());
  }
  if (query.limit && Number.isFinite(query.limit)) {
    qs.set("limit", `${query.limit}`);
  }
  const url = qs.toString() ? `${base}/api/v1/approvals?${qs.toString()}` : `${base}/api/v1/approvals`;
  const r = await fetch(url, daemonFetchInit(auth));
  const j = await parseJsonOrThrow(r);
  return ApprovalsListSchema.parse(j);
}

export async function apiGetApproval(
  base: string,
  approvalId: string,
  auth?: ApiAuth,
): Promise<ApprovalDetailResp> {
  const r = await fetch(`${base}/api/v1/approvals/${approvalId}`, daemonFetchInit(auth));
  const j = await parseJsonOrThrow(r);
  return ApprovalDetailSchema.parse(j);
}

export async function apiPostApprovalDecision(
  base: string,
  approvalId: string,
  req: ApprovalDecisionReq,
  auth?: ApiAuth,
): Promise<ApprovalDecisionResp> {
  const payload = {
    member_id: req.memberId,
    member_role: req.memberRole ?? "",
    decision: req.decision,
    note: req.note ?? "",
  };
  const r = await fetch(
    `${base}/api/v1/approvals/${approvalId}/decisions`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(payload) }, { "Content-Type": "application/json" }),
  );
  const j = await parseJsonOrThrow(r);
  return ApprovalDecisionRespSchema.parse(j);
}
