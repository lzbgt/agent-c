import { z } from "zod";

export const MemoryQueryRespSchema = z
  .object({
    ok: z.boolean(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type MemoryQueryResp = z.infer<typeof MemoryQueryRespSchema>;

export const MemoryCorrelateRespSchema = z
  .object({
    ok: z.boolean(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type MemoryCorrelateResp = z.infer<typeof MemoryCorrelateRespSchema>;

export const MemoryCheckpointsRespSchema = z
  .object({
    ok: z.boolean(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type MemoryCheckpointsResp = z.infer<typeof MemoryCheckpointsRespSchema>;

export const MemoryIndexRespSchema = z
  .object({
    ok: z.boolean(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type MemoryIndexResp = z.infer<typeof MemoryIndexRespSchema>;

export const MemorySalienceRespSchema = z
  .object({
    ok: z.boolean(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type MemorySalienceResp = z.infer<typeof MemorySalienceRespSchema>;

export const MemoryRetentionRespSchema = z
  .object({
    ok: z.boolean(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type MemoryRetentionResp = z.infer<typeof MemoryRetentionRespSchema>;

export const MemoryRecapsRespSchema = z
  .object({
    ok: z.boolean(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type MemoryRecapsResp = z.infer<typeof MemoryRecapsRespSchema>;
