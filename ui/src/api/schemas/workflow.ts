import { z } from "zod";
import type {
  WorkflowBudgetSnapshot,
  WorkflowJsonObject,
  WorkflowJsonValue,
} from "../../workflowTypes";

const WorkflowJsonValueSchema: z.ZodType<WorkflowJsonValue> = z.lazy(() =>
  z.union([
    z.string(),
    z.number(),
    z.boolean(),
    z.null(),
    z.array(WorkflowJsonValueSchema),
    z.record(z.string(), WorkflowJsonValueSchema),
  ]),
);

const WorkflowJsonObjectSchema: z.ZodType<WorkflowJsonObject> = z.record(z.string(), WorkflowJsonValueSchema);

const WorkflowBudgetSnapshotSchema: z.ZodType<WorkflowBudgetSnapshot> = WorkflowJsonObjectSchema;

const WorkflowSummarySchema = z
  .object({
    workflow_id: z.string(),
    status: z.string().optional(),
    priority: z.number().optional(),
    deadline_unix_ms: z.number().optional(),
    idempotency_key: z.string().optional(),
    trace_id: z.string().optional(),
    session_id: z.string().optional(),
    cancel_requested: z.boolean().optional(),
    error: z.string().optional(),
    created_unix_ms: z.number().optional(),
    updated_unix_ms: z.number().optional(),
  })
  .passthrough();

const WorkflowTaskSchema = z
  .object({
    task_id: z.string(),
    status: z.string().optional(),
    depends_on: z.array(z.string()).optional(),
    allow_error: z.boolean().optional(),
    attempt: z.number().optional(),
    max_attempts: z.number().optional(),
    error: z.string().optional(),
    ready_unix_ms: z.number().optional(),
    started_unix_ms: z.number().optional(),
    finished_unix_ms: z.number().optional(),
  })
  .passthrough();

export const WorkflowListRespSchema = z
  .object({
    ok: z.boolean(),
    status: z.string().optional(),
    workflows: z.array(WorkflowSummarySchema).optional(),
    error: z.string().optional(),
    detail: z.string().optional(),
  })
  .passthrough();

export const WorkflowDetailRespSchema = z
  .object({
    ok: z.boolean().optional(),
    workflow: WorkflowSummarySchema.optional(),
    tasks: z.array(WorkflowTaskSchema).optional(),
    workflow_limits: WorkflowBudgetSnapshotSchema.optional(),
    workflow_usage: WorkflowBudgetSnapshotSchema.optional(),
    workflow_remaining: WorkflowBudgetSnapshotSchema.optional(),
    result: WorkflowJsonObjectSchema.optional(),
    spec: WorkflowJsonObjectSchema.optional(),
    spec_json: z.string().optional(),
    error: z.string().optional(),
    detail: z.string().optional(),
  })
  .passthrough();

export const WorkflowSubmitRespSchema = z
  .object({
    ok: z.boolean().optional(),
    workflow_id: z.string().optional(),
    trace_id: z.string().optional(),
    session_id: z.string().optional(),
    deduped: z.boolean().optional(),
    error: z.string().optional(),
    detail: z.string().optional(),
  })
  .passthrough();

export const WorkflowCancelRespSchema = z
  .object({
    ok: z.boolean().optional(),
    workflow_id: z.string().optional(),
    error: z.string().optional(),
    detail: z.string().optional(),
  })
  .passthrough();

export type WorkflowListResp = z.infer<typeof WorkflowListRespSchema>;
export type WorkflowDetailResp = z.infer<typeof WorkflowDetailRespSchema>;
export type WorkflowSubmitResp = z.infer<typeof WorkflowSubmitRespSchema>;
export type WorkflowCancelResp = z.infer<typeof WorkflowCancelRespSchema>;
