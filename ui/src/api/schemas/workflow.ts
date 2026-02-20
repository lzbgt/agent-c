import { z } from "zod";

export const WorkflowListRespSchema = z
  .object({
    ok: z.boolean(),
    status: z.string().optional(),
    workflows: z.array(z.any()).optional(),
    error: z.string().optional(),
    detail: z.string().optional(),
  })
  .passthrough();
export type WorkflowListResp = z.infer<typeof WorkflowListRespSchema>;

export const WorkflowDetailRespSchema = z
  .object({
    ok: z.boolean().optional(),
    workflow: z.any().optional(),
    tasks: z.array(z.any()).optional(),
    workflow_limits: z.any().optional(),
    workflow_usage: z.any().optional(),
    workflow_remaining: z.any().optional(),
    result: z.any().optional(),
    spec: z.any().optional(),
    spec_json: z.string().optional(),
    error: z.string().optional(),
    detail: z.string().optional(),
  })
  .passthrough();
export type WorkflowDetailResp = z.infer<typeof WorkflowDetailRespSchema>;

export const WorkflowSubmitRespSchema = z
  .object({
    ok: z.boolean().optional(),
    workflow_id: z.string().optional(),
    trace_id: z.string().optional(),
    deduped: z.boolean().optional(),
    error: z.string().optional(),
    detail: z.string().optional(),
  })
  .passthrough();
export type WorkflowSubmitResp = z.infer<typeof WorkflowSubmitRespSchema>;

export const WorkflowCancelRespSchema = z
  .object({
    ok: z.boolean().optional(),
    workflow_id: z.string().optional(),
    error: z.string().optional(),
    detail: z.string().optional(),
  })
  .passthrough();
export type WorkflowCancelResp = z.infer<typeof WorkflowCancelRespSchema>;
