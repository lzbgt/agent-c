import { z } from "zod";

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
export type WorkflowScheduleCreateResp = z.infer<typeof WorkflowScheduleCreateRespSchema>;

export const WorkflowScheduleListRespSchema = z
  .object({
    ok: z.boolean().optional(),
    status: z.string().nullable().optional(),
    schedules: z.array(z.any()).optional(),
    error: z.string().optional(),
    detail: z.string().optional(),
  })
  .passthrough();
export type WorkflowScheduleListResp = z.infer<typeof WorkflowScheduleListRespSchema>;

export const WorkflowScheduleGetRespSchema = z
  .object({
    ok: z.boolean().optional(),
    schedule: z.any().optional(),
    error: z.string().optional(),
    detail: z.string().optional(),
  })
  .passthrough();
export type WorkflowScheduleGetResp = z.infer<typeof WorkflowScheduleGetRespSchema>;

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
export type WorkflowScheduleUpdateResp = z.infer<typeof WorkflowScheduleUpdateRespSchema>;

export const WorkflowScheduleRunsRespSchema = z
  .object({
    ok: z.boolean().optional(),
    schedule_id: z.string().optional(),
    runs: z.array(z.any()).optional(),
    error: z.string().optional(),
    detail: z.string().optional(),
  })
  .passthrough();
export type WorkflowScheduleRunsResp = z.infer<typeof WorkflowScheduleRunsRespSchema>;
