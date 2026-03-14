import { z } from "zod";

export const SessionAttachmentSchema = z
  .object({
    client_id: z.string().nullable().optional(),
    lease_seconds: z.number().nullable().optional(),
    lease_expires_at_ms: z.number().nullable().optional(),
    lease_active: z.boolean().optional(),
  })
  .passthrough();
export type SessionAttachment = z.infer<typeof SessionAttachmentSchema>;

export const SessionInfoSchema = z
  .object({
    session_id: z.string().optional(),
    thread_id: z.string().nullable().optional(),
    working: z.boolean().optional(),
    last_turn_id: z.string().nullable().optional(),
    attachment: SessionAttachmentSchema.optional(),
    messages: z
      .array(
        z.object({
          role: z.string().optional(),
          content: z.string().optional(),
          mm_json: z.string().optional(),
          mm_bytes: z.number().optional(),
          mm_truncated: z.number().optional(),
        }),
      )
      .optional(),
  })
  .passthrough();
export type SessionInfo = z.infer<typeof SessionInfoSchema>;

export const SessionErrorEnvelopeSchema = z
  .object({
    code: z.string().optional(),
    message: z.string().optional(),
    retryable: z.boolean().optional(),
    details: z.record(z.any()).optional(),
  })
  .passthrough();
export type SessionErrorEnvelope = z.infer<typeof SessionErrorEnvelopeSchema>;

export const SessionErrorValueSchema = z.union([z.string(), SessionErrorEnvelopeSchema]);
export type SessionErrorValue = z.infer<typeof SessionErrorValueSchema>;

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
    error: SessionErrorValueSchema.optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type SessionUploadResp = z.infer<typeof SessionUploadRespSchema>;

export const SessionsSchema = z.object({
  ok: z.boolean(),
  sessions: z.array(z.union([z.string(), SessionInfoSchema])).optional(),
  error: SessionErrorValueSchema.optional(),
  err: z.string().optional(),
  code: z.string().optional(),
  session: SessionInfoSchema.optional(),
  status: z.number().optional(),
})
  .passthrough();
export type SessionsResp = z.infer<typeof SessionsSchema>;

export const SessionSchema = z.object({
  ok: z.boolean(),
  session_id: z.string().optional(),
  messages: SessionInfoSchema.shape.messages,
  session: SessionInfoSchema.optional(),
  error: SessionErrorValueSchema.optional(),
  err: z.string().optional(),
  code: z.string().optional(),
  status: z.number().optional(),
  error_detail: SessionErrorEnvelopeSchema.optional(),
})
  .passthrough();
export type SessionResp = z.infer<typeof SessionSchema>;

export const SessionSceneSchema = z
  .object({
    ok: z.boolean(),
    session_id: z.string().optional(),
    updated_unix_ms: z.number().optional(),
    scene: z.record(z.any()).optional(),
    error: SessionErrorValueSchema.optional(),
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
    error: SessionErrorValueSchema.optional(),
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
    error: SessionErrorValueSchema.optional(),
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
    session: SessionInfoSchema.optional(),
    error: SessionErrorValueSchema.optional(),
    err: z.string().optional(),
    code: z.string().optional(),
    status: z.number().optional(),
    error_detail: SessionErrorEnvelopeSchema.optional(),
    error_details: z.record(z.any()).optional(),
  })
  .passthrough();
export type NewSessionResp = z.infer<typeof NewSessionRespSchema>;

export const DeleteSessionRespSchema = z
  .object({
    ok: z.boolean(),
    session_id: z.string().optional(),
    error: SessionErrorValueSchema.optional(),
    err: z.string().optional(),
    code: z.string().optional(),
    status: z.number().optional(),
  })
  .passthrough();
export type DeleteSessionResp = z.infer<typeof DeleteSessionRespSchema>;

export const AuditSchema = z.object({
  ok: z.boolean(),
  session_id: z.string().optional(),
  entries: z.array(z.any()).optional(),
  error: SessionErrorValueSchema.optional(),
  err: z.string().optional(),
  code: z.string().optional(),
  status: z.number().optional(),
})
  .passthrough();
export type AuditResp = z.infer<typeof AuditSchema>;

export const SessionClientEventsSchema = z.object({
  ok: z.boolean(),
  session_id: z.string().optional(),
  max_bytes: z.number().optional(),
  count: z.number().optional(),
  events: z.array(z.any()).optional(),
  error: SessionErrorValueSchema.optional(),
  err: z.string().optional(),
  code: z.string().optional(),
  status: z.number().optional(),
})
  .passthrough();
export type SessionClientEventsResp = z.infer<typeof SessionClientEventsSchema>;

export const SessionArtifactsSchema = z.object({
  ok: z.boolean(),
  session_id: z.string().optional(),
  count: z.number().optional(),
  artifacts: z.array(z.any()).optional(),
  error: SessionErrorValueSchema.optional(),
  err: z.string().optional(),
  code: z.string().optional(),
  status: z.number().optional(),
})
  .passthrough();
export type SessionArtifactsResp = z.infer<typeof SessionArtifactsSchema>;

export const SessionAttachmentActionRespSchema = z
  .object({
    ok: z.boolean(),
    session_id: z.string().optional(),
    session: SessionInfoSchema.optional(),
    attachment: SessionAttachmentSchema.optional(),
    operation: z.record(z.any()).optional(),
    error: SessionErrorValueSchema.optional(),
    err: z.string().optional(),
    code: z.string().optional(),
    status: z.number().optional(),
    error_detail: SessionErrorEnvelopeSchema.optional(),
  })
  .passthrough();
export type SessionAttachmentActionResp = z.infer<typeof SessionAttachmentActionRespSchema>;

export const SessionOperatorRespSchema = z
  .object({
    ok: z.boolean().optional(),
    error: SessionErrorValueSchema.optional(),
    err: z.string().optional(),
    code: z.string().optional(),
    status: z.number().optional(),
    message: z.string().optional(),
  })
  .passthrough();
export type SessionOperatorResp = z.infer<typeof SessionOperatorRespSchema>;
