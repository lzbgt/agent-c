import { z } from "zod";

export const HealthSchema = z.object({
  ok: z.boolean(),
  service: z.string().optional(),
  version: z.string().optional(),
});
export type Health = z.infer<typeof HealthSchema>;

export const RunRequestSchema = z.object({
  prompt: z.string().min(1),
  session_id: z.string().optional(),
  no_session: z.boolean().optional(),
  model: z.string().optional(),
  base_url: z.string().optional(),
  api_key: z.string().optional(),
  tools: z.enum(["host", "basic", "none"]).optional(),
  tools_root: z.string().optional(),
  max_steps: z.number().int().nonnegative().optional(),
  trace: z.boolean().optional(),
  yolo: z.boolean().optional(),
  verbose: z.boolean().optional(),
});
export type RunRequest = z.infer<typeof RunRequestSchema>;

export const EventSchema = z.object({
  type: z.string(),
  data: z.any().optional(),
});
export type AgentEvent = z.infer<typeof EventSchema>;

export const RunResponseSchema = z.object({
  ok: z.boolean(),
  assistant_text: z.string().optional(),
  error: z.string().optional(),
  http_status: z.number().optional(),
  http_body: z.string().optional(),
  trace_text: z.string().optional(),
  effective_tools_root: z.string().optional(),
  effective_yolo: z.boolean().optional(),
  verbose: z.boolean().optional(),
  events: z.array(EventSchema).optional(),
});
export type RunResponse = z.infer<typeof RunResponseSchema>;

export async function apiGetHealth(base: string): Promise<Health> {
  const r = await fetch(`${base}/api/v1/health`);
  const j = await r.json();
  return HealthSchema.parse(j);
}

export const SessionsSchema = z.object({
  ok: z.boolean(),
  sessions: z.array(z.string()).optional(),
  error: z.string().optional(),
});
export type SessionsResp = z.infer<typeof SessionsSchema>;

export async function apiListSessions(base: string): Promise<SessionsResp> {
  const r = await fetch(`${base}/api/v1/sessions`);
  const j = await r.json();
  return SessionsSchema.parse(j);
}

export const SessionSchema = z.object({
  ok: z.boolean(),
  session_id: z.string().optional(),
  messages: z.array(z.object({ role: z.string(), content: z.string() })).optional(),
  error: z.string().optional(),
});
export type SessionResp = z.infer<typeof SessionSchema>;

export async function apiGetSession(base: string, sessionId: string): Promise<SessionResp> {
  const r = await fetch(`${base}/api/v1/session?session_id=${encodeURIComponent(sessionId)}`);
  const j = await r.json();
  return SessionSchema.parse(j);
}

export const AuditSchema = z.object({
  ok: z.boolean(),
  session_id: z.string().optional(),
  entries: z.array(z.any()).optional(),
  error: z.string().optional(),
});
export type AuditResp = z.infer<typeof AuditSchema>;

export async function apiGetAudit(base: string, sessionId: string): Promise<AuditResp> {
  const r = await fetch(`${base}/api/v1/session/audit?session_id=${encodeURIComponent(sessionId)}&max_bytes=1048576`);
  const j = await r.json();
  return AuditSchema.parse(j);
}

export async function apiRun(base: string, req: RunRequest): Promise<RunResponse> {
  const payload = RunRequestSchema.parse(req);
  const r = await fetch(`${base}/api/v1/run`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(payload),
  });
  const j = await r.json();
  return RunResponseSchema.parse(j);
}
