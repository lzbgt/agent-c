import { z } from "zod";
import { daemonFetchInit, type ApiAuth } from "./auth";
const safeInteger = z.number().int().min(0).max(Number.MAX_SAFE_INTEGER);
export const SessionPageSchema = z.object({
  ok: z.literal(true), session_id: z.string(),
  messages: z.array(z.object({
    index: safeInteger, role: z.string(), content: z.string(),
    mm_json: z.string().optional(), mm_bytes: safeInteger.optional(), mm_truncated: safeInteger.optional(),
  })).max(200),
  page: z.object({ revision: safeInteger, has_more: z.boolean(), next_before_index: safeInteger.nullable() }),
});
export type SessionPage = z.infer<typeof SessionPageSchema>;
export class SessionPageError extends Error {
  constructor(message: string, public status: number) { super(message); this.name = "SessionPageError"; }
}
// Pages are generation-scoped snapshots, not append-only message history.
// On 409 discard accumulated pages and start again; never merge generations.
export async function apiGetSessionPage(base: string, sessionId: string,
  options: { limit?: number; beforeIndex?: number; revision?: number; signal?: AbortSignal } = {},
  auth?: ApiAuth): Promise<SessionPage> {
  const limit = options.limit ?? 100;
  if (!Number.isInteger(limit) || limit < 1 || limit > 200 ||
      (options.beforeIndex !== undefined && (options.revision === undefined || !Number.isSafeInteger(options.beforeIndex) || options.beforeIndex < 0)) ||
      (options.revision !== undefined && (!Number.isSafeInteger(options.revision) || options.revision < 0))) throw new Error("Invalid session page cursor");
  const query = new URLSearchParams({ session_id: sessionId, messages_limit: String(limit) });
  if (options.beforeIndex !== undefined) query.set("messages_before_index", String(options.beforeIndex));
  if (options.revision !== undefined) query.set("messages_revision", String(options.revision));
  const response = await fetch(base + "/api/v1/session?" + query, daemonFetchInit(auth, { signal: options.signal }));
  const body = await response.json();
  if (!response.ok) throw new SessionPageError(body.error || "Session page failed", response.status);
  const page = SessionPageSchema.parse(body);
  if (page.session_id !== sessionId || (options.revision !== undefined && page.page.revision !== options.revision))
    throw new SessionPageError("Session page identity changed", 409);
  if (page.page.has_more && (page.messages.length === 0 || page.page.next_before_index !== page.messages[0].index))
    throw new Error("Invalid session page continuation");
  for (let i = 0; i < page.messages.length; i++) {
    if ((i > 0 && page.messages[i].index <= page.messages[i - 1].index) ||
        (options.beforeIndex !== undefined && page.messages[i].index >= options.beforeIndex))
      throw new Error("Invalid session page ordering");
  }
  return page;
}
