import { z } from "zod";

const UnknownValueSchema = z.unknown();
const UnknownArraySchema = z.array(UnknownValueSchema);

export const DbRunsSchema = z.object({
  ok: z.boolean(),
  session_id: z.string().optional(),
  limit: z.number().optional(),
  offset: z.number().optional(),
  count: z.number().optional(),
  runs: UnknownArraySchema.optional(),
  error: z.string().optional(),
  err: z.string().optional(),
  code: z.string().optional(),
});
export type DbRunsResp = z.infer<typeof DbRunsSchema>;

export const DbRunSchema = z.object({
  ok: z.boolean(),
  run: UnknownValueSchema.optional(),
  events: UnknownArraySchema.optional(),
  tool_records: UnknownArraySchema.optional(),
  artifacts: UnknownArraySchema.optional(),
  ui_actions: UnknownArraySchema.optional(),
  error: z.string().optional(),
  err: z.string().optional(),
  code: z.string().optional(),
});
export type DbRunResp = z.infer<typeof DbRunSchema>;

export const DbArtifactsSchema = z.object({
  ok: z.boolean(),
  session_id: z.string().optional(),
  limit: z.number().optional(),
  offset: z.number().optional(),
  count: z.number().optional(),
  artifacts: UnknownArraySchema.optional(),
  error: z.string().optional(),
  err: z.string().optional(),
  code: z.string().optional(),
});
export type DbArtifactsResp = z.infer<typeof DbArtifactsSchema>;

export const DbUiActionsSchema = z.object({
  ok: z.boolean(),
  session_id: z.string().optional(),
  limit: z.number().optional(),
  offset: z.number().optional(),
  count: z.number().optional(),
  ui_actions: UnknownArraySchema.optional(),
  error: z.string().optional(),
  err: z.string().optional(),
  code: z.string().optional(),
});
export type DbUiActionsResp = z.infer<typeof DbUiActionsSchema>;

export const DbSessionsSchema = z.object({
  ok: z.boolean(),
  limit: z.number().optional(),
  offset: z.number().optional(),
  count: z.number().optional(),
  sessions: UnknownArraySchema.optional(),
  error: z.string().optional(),
  err: z.string().optional(),
  code: z.string().optional(),
});
export type DbSessionsResp = z.infer<typeof DbSessionsSchema>;

export const DbMessagesSchema = z.object({
  ok: z.boolean(),
  session_id: z.string().optional(),
  limit: z.number().optional(),
  offset: z.number().optional(),
  max_content_bytes: z.number().optional(),
  max_mm_bytes: z.number().optional(),
  count: z.number().optional(),
  messages: UnknownArraySchema.optional(),
  error: z.string().optional(),
  err: z.string().optional(),
  code: z.string().optional(),
});
export type DbMessagesResp = z.infer<typeof DbMessagesSchema>;

export const DbClientEventsSchema = z.object({
  ok: z.boolean(),
  session_id: z.string().optional(),
  limit: z.number().optional(),
  offset: z.number().optional(),
  count: z.number().optional(),
  client_events: UnknownArraySchema.optional(),
  error: z.string().optional(),
  err: z.string().optional(),
  code: z.string().optional(),
});
export type DbClientEventsResp = z.infer<typeof DbClientEventsSchema>;
