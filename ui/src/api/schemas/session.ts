import { z } from "zod";

export const SessionUploadReqSchema = z
  .object({
    session_id: z.string().min(1),
    files: z
      .array(
        z.object({
          name: z.string().min(1),
          mime: z.string().optional(),
          data_base64: z.string().min(1),
        }),
      )
      .min(1),
  })
  .passthrough();
export type SessionUploadReq = z.infer<typeof SessionUploadReqSchema>;

export const SessionUploadRespSchema = z
  .object({
    ok: z.boolean(),
    session_id: z.string().optional(),
    files: z
      .array(
        z
          .object({
            name: z.string().optional(),
            mime: z.string().optional(),
            kind: z.string().optional(),
            path: z.string().optional(),
            bytes: z.number().optional(),
          })
          .passthrough(),
      )
      .optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type SessionUploadResp = z.infer<typeof SessionUploadRespSchema>;

export const SessionsSchema = z.object({
  ok: z.boolean(),
  sessions: z.array(z.string()).optional(),
  error: z.string().optional(),
  err: z.string().optional(),
  code: z.string().optional(),
});
export type SessionsResp = z.infer<typeof SessionsSchema>;

export const SessionSchema = z.object({
  ok: z.boolean(),
  session_id: z.string().optional(),
  messages: z
    .array(
      z.object({
        role: z.string(),
        content: z.string(),
        mm_json: z.string().optional(),
        mm_bytes: z.number().optional(),
        mm_truncated: z.number().optional(),
      }),
    )
    .optional(),
  error: z.string().optional(),
  err: z.string().optional(),
  code: z.string().optional(),
});
export type SessionResp = z.infer<typeof SessionSchema>;

export const SessionSceneSchema = z
  .object({
    ok: z.boolean(),
    session_id: z.string().optional(),
    updated_unix_ms: z.number().optional(),
    scene: z.record(z.any()).optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type SessionSceneResp = z.infer<typeof SessionSceneSchema>;

export const SessionSceneApplyReqSchema = z.object({
  session_id: z.string().min(1),
  ops: z.array(z.any()),
});
export type SessionSceneApplyReq = z.infer<typeof SessionSceneApplyReqSchema>;

export const SessionSceneApplyRespSchema = z
  .object({
    ok: z.boolean(),
    session_id: z.string().optional(),
    updated_unix_ms: z.number().optional(),
    apply: z.any().optional(),
    scene: z.record(z.any()).optional(),
    warning: z.string().optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type SessionSceneApplyResp = z.infer<typeof SessionSceneApplyRespSchema>;

export const SessionUiEventReqSchema = z.object({
  session_id: z.string().min(1),
  type: z.string().min(1),
  ts_unix_ms: z.number().optional(),
  // Optional client identity (collaboration protocol).
  client: z
    .object({
      id: z.string().optional(),
      kind: z.string().optional(),
      instance_id: z.string().optional(),
    })
    .optional(),
  data: z.any().optional(),
  append_to_session: z.boolean().optional(),
});
export type SessionUiEventReq = z.infer<typeof SessionUiEventReqSchema>;

export const SessionUiEventRespSchema = z
  .object({
    ok: z.boolean(),
    session_id: z.string().optional(),
    type: z.string().optional(),
    appended_to_session: z.boolean().optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type SessionUiEventResp = z.infer<typeof SessionUiEventRespSchema>;

export const NewSessionRespSchema = z
  .object({
    ok: z.boolean(),
    session_id: z.string().optional(),
    created: z.boolean().optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type NewSessionResp = z.infer<typeof NewSessionRespSchema>;

export const DeleteSessionRespSchema = z
  .object({
    ok: z.boolean(),
    session_id: z.string().optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type DeleteSessionResp = z.infer<typeof DeleteSessionRespSchema>;

export const AuditSchema = z.object({
  ok: z.boolean(),
  session_id: z.string().optional(),
  entries: z.array(z.any()).optional(),
  error: z.string().optional(),
  err: z.string().optional(),
  code: z.string().optional(),
});
export type AuditResp = z.infer<typeof AuditSchema>;

export const SessionClientEventsSchema = z.object({
  ok: z.boolean(),
  session_id: z.string().optional(),
  max_bytes: z.number().optional(),
  count: z.number().optional(),
  events: z.array(z.any()).optional(),
  error: z.string().optional(),
  err: z.string().optional(),
  code: z.string().optional(),
});
export type SessionClientEventsResp = z.infer<typeof SessionClientEventsSchema>;

export const SessionArtifactsSchema = z.object({
  ok: z.boolean(),
  session_id: z.string().optional(),
  count: z.number().optional(),
  artifacts: z.array(z.any()).optional(),
  error: z.string().optional(),
  err: z.string().optional(),
  code: z.string().optional(),
});
export type SessionArtifactsResp = z.infer<typeof SessionArtifactsSchema>;
