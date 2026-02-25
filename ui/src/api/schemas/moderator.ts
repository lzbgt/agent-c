import { z } from "zod";

export const ModeratorActorSchema = z
  .object({
    role: z.string().optional(),
    id: z.string().optional(),
    kind: z.string().optional(),
    instance_id: z.string().optional(),
  })
  .passthrough();

export const ModeratorDirectiveReqSchema = z.object({
  session_id: z.string(),
  directive: z.string(),
  scope: z.string().optional(),
  assignees: z.array(z.string()).optional(),
  priority: z.number().int().optional(),
  metadata: z.record(z.any()).optional(),
  actor: ModeratorActorSchema.optional(),
  append_to_session: z.boolean().optional(),
  ts_unix_ms: z.number().int().optional(),
});
export type ModeratorDirectiveReq = z.infer<typeof ModeratorDirectiveReqSchema>;

export const ModeratorTaskReqSchema = z.object({
  session_id: z.string(),
  title: z.string(),
  detail: z.string().optional(),
  priority: z.number().int().optional(),
  tags: z.array(z.string()).optional(),
  assignees: z.array(z.string()).optional(),
  metadata: z.record(z.any()).optional(),
  status: z.string().optional(),
  actor: ModeratorActorSchema.optional(),
  append_to_session: z.boolean().optional(),
  ts_unix_ms: z.number().int().optional(),
});
export type ModeratorTaskReq = z.infer<typeof ModeratorTaskReqSchema>;

export const ModeratorEventSchema = z
  .object({
    type: z.string().optional(),
    ts_unix_ms: z.number().optional(),
    actor: ModeratorActorSchema.optional(),
    data: z.any().optional(),
  })
  .passthrough();
export type ModeratorEvent = z.infer<typeof ModeratorEventSchema>;

export const ModeratorPostRespSchema = z.object({
  ok: z.boolean(),
  session_id: z.string().optional(),
  type: z.string().optional(),
  appended_to_session: z.boolean().optional(),
  logged_to_client_events: z.boolean().optional(),
  event: ModeratorEventSchema.optional(),
  error: z.string().optional(),
});
export type ModeratorPostResp = z.infer<typeof ModeratorPostRespSchema>;

export const ModeratorEventsRespSchema = z.object({
  ok: z.boolean(),
  session_id: z.string().optional(),
  max_bytes: z.number().optional(),
  count: z.number().optional(),
  events: z.array(ModeratorEventSchema).optional(),
  error: z.string().optional(),
});
export type ModeratorEventsResp = z.infer<typeof ModeratorEventsRespSchema>;
