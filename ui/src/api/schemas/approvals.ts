import { z } from "zod";

export const ApprovalDecisionSchema = z.object({
  id: z.number().optional(),
  approval_id: z.string().optional(),
  member_id: z.string().optional(),
  decision: z.string().optional(),
  decision_unix_ms: z.number().optional(),
  note: z.string().optional(),
});
export type ApprovalDecision = z.infer<typeof ApprovalDecisionSchema>;

export const ApprovalRequestSchema = z.object({
  approval_id: z.string().optional(),
  run_id: z.number().optional(),
  trace_id: z.string().optional(),
  session_id: z.string().optional(),
  job_id: z.string().optional(),
  team_id: z.string().optional(),
  tool_name: z.string().optional(),
  tool_call_id: z.string().optional(),
  tool_args_hash: z.string().optional(),
  status: z.string().optional(),
  required_approvals: z.number().optional(),
  role_constraints: z.array(z.string()).optional(),
  created_unix_ms: z.number().optional(),
  expires_unix_ms: z.number().optional(),
  decision_reason: z.string().optional(),
});
export type ApprovalRequest = z.infer<typeof ApprovalRequestSchema>;

export const ApprovalsListSchema = z.object({
  ok: z.boolean(),
  approvals: z.array(ApprovalRequestSchema).optional(),
  limit: z.number().optional(),
  error: z.string().optional(),
  err: z.string().optional(),
  code: z.string().optional(),
});
export type ApprovalsListResp = z.infer<typeof ApprovalsListSchema>;

export const ApprovalDetailSchema = z.object({
  ok: z.boolean(),
  approval: ApprovalRequestSchema.optional(),
  decisions: z.array(ApprovalDecisionSchema).optional(),
  error: z.string().optional(),
  err: z.string().optional(),
  code: z.string().optional(),
});
export type ApprovalDetailResp = z.infer<typeof ApprovalDetailSchema>;

export const ApprovalDecisionRespSchema = z.object({
  ok: z.boolean(),
  approval_id: z.string().optional(),
  status: z.string().optional(),
  approved: z.number().optional(),
  required_approvals: z.number().optional(),
  decision: ApprovalDecisionSchema.optional(),
  error: z.string().optional(),
  err: z.string().optional(),
  code: z.string().optional(),
});
export type ApprovalDecisionResp = z.infer<typeof ApprovalDecisionRespSchema>;
