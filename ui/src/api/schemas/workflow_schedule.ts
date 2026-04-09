import { z } from "zod";

const WorkflowScheduleSchema = z
  .object({
    schedule_id: z.string(),
    status: z.enum(["active", "paused", "error"]),
    cron: z.string(),
    timezone: z.string(),
    created_unix_ms: z.number(),
    updated_unix_ms: z.number(),
    last_tick_unix_ms: z.number().optional(),
    next_tick_unix_ms: z.number().optional(),
    last_error: z.string().optional(),
    metadata: z.record(z.string(), z.unknown()).optional(),
  })
  .passthrough();

const WorkflowScheduleRunSchema = z
  .object({
    schedule_id: z.string(),
    tick_unix_ms: z.number(),
    workflow_id: z.string(),
    created_unix_ms: z.number(),
    status: z.string(),
    error: z.string().optional(),
  })
  .passthrough();

export const WorkflowScheduleCreateRespSchema = z
  .object({
    ok: z.boolean().optional(),
    schedule_id: z.string().optional(),
    status: z.string().optional(),
    next_tick_unix_ms: z.number().optional(),
    error: z.string().optional(),
    detail: z.string().optional(),
  })
  .passthrough();

export const WorkflowScheduleListRespSchema = z
  .object({
    ok: z.boolean().optional(),
    status: z.string().nullable().optional(),
    schedules: z.array(WorkflowScheduleSchema).optional(),
    error: z.string().optional(),
    detail: z.string().optional(),
  })
  .passthrough();

export const WorkflowScheduleGetRespSchema = z
  .object({
    ok: z.boolean().optional(),
    schedule: WorkflowScheduleSchema.optional(),
    error: z.string().optional(),
    detail: z.string().optional(),
  })
  .passthrough();

export const WorkflowScheduleUpdateRespSchema = z
  .object({
    ok: z.boolean().optional(),
    schedule_id: z.string().optional(),
    status: z.string().optional(),
    next_tick_unix_ms: z.number().optional(),
    error: z.string().optional(),
    detail: z.string().optional(),
  })
  .passthrough();

export const WorkflowScheduleRunsRespSchema = z
  .object({
    ok: z.boolean().optional(),
    schedule_id: z.string().optional(),
    runs: z.array(WorkflowScheduleRunSchema).optional(),
    error: z.string().optional(),
    detail: z.string().optional(),
  })
  .passthrough();

export type WorkflowScheduleCreateResp = z.infer<typeof WorkflowScheduleCreateRespSchema>;
export type WorkflowScheduleListResp = z.infer<typeof WorkflowScheduleListRespSchema>;
export type WorkflowScheduleGetResp = z.infer<typeof WorkflowScheduleGetRespSchema>;
export type WorkflowScheduleUpdateResp = z.infer<typeof WorkflowScheduleUpdateRespSchema>;
export type WorkflowScheduleRunsResp = z.infer<typeof WorkflowScheduleRunsRespSchema>;
