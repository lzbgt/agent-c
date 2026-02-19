import { daemonHeaders, type ApiAuth } from "./auth";
import {
  DbArtifactsSchema,
  type DbArtifactsResp,
  DbClientEventsSchema,
  type DbClientEventsResp,
  DbMessagesSchema,
  type DbMessagesResp,
  DbRunSchema,
  type DbRunResp,
  DbRunsSchema,
  type DbRunsResp,
  DbSessionsSchema,
  type DbSessionsResp,
  DbUiActionsSchema,
  type DbUiActionsResp,
} from "./schemas/db";

export async function apiGetDbRuns(
  base: string,
  sessionId: string,
  auth?: ApiAuth,
  opts?: { limit?: number; offset?: number; onlyErrors?: boolean; stopReason?: string },
): Promise<DbRunsResp> {
  const limit = typeof opts?.limit === "number" ? opts.limit : 50;
  const offset = typeof opts?.offset === "number" ? opts.offset : 0;
  const onlyErrors = !!opts?.onlyErrors;
  const stopReason = typeof opts?.stopReason === "string" ? opts.stopReason : "";
  const r = await fetch(
    `${base}/api/v1/db/runs?session_id=${encodeURIComponent(sessionId)}&limit=${encodeURIComponent(
      String(limit),
    )}&offset=${encodeURIComponent(String(offset))}&only_errors=${encodeURIComponent(onlyErrors ? "1" : "0")}${
      stopReason.trim().length > 0 ? `&stop_reason=${encodeURIComponent(stopReason.trim())}` : ""
    }`,
    { headers: daemonHeaders(auth) },
  );
  const j = await r.json();
  return DbRunsSchema.parse(j);
}

export async function apiGetDbRun(
  base: string,
  runId: number,
  auth?: ApiAuth,
  opts?: { includeEvents?: boolean; includeTools?: boolean; includeArtifacts?: boolean; includeUiActions?: boolean },
): Promise<DbRunResp> {
  const q = new URLSearchParams();
  q.set("run_id", String(runId));
  if (opts?.includeEvents) q.set("include_events", "1");
  if (opts?.includeTools) q.set("include_tools", "1");
  if (opts?.includeArtifacts) q.set("include_artifacts", "1");
  if (opts?.includeUiActions) q.set("include_ui_actions", "1");
  const r = await fetch(`${base}/api/v1/db/run?${q.toString()}`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return DbRunSchema.parse(j);
}

export async function apiGetDbArtifacts(
  base: string,
  sessionId: string,
  auth?: ApiAuth,
  opts?: { limit?: number; offset?: number },
): Promise<DbArtifactsResp> {
  const limit = typeof opts?.limit === "number" ? opts.limit : 50;
  const offset = typeof opts?.offset === "number" ? opts.offset : 0;
  const r = await fetch(
    `${base}/api/v1/db/artifacts?session_id=${encodeURIComponent(sessionId)}&limit=${encodeURIComponent(
      String(limit),
    )}&offset=${encodeURIComponent(String(offset))}`,
    { headers: daemonHeaders(auth) },
  );
  const j = await r.json();
  return DbArtifactsSchema.parse(j);
}

export async function apiGetDbUiActions(
  base: string,
  sessionId: string,
  auth?: ApiAuth,
  opts?: { limit?: number; offset?: number },
): Promise<DbUiActionsResp> {
  const limit = typeof opts?.limit === "number" ? opts.limit : 50;
  const offset = typeof opts?.offset === "number" ? opts.offset : 0;
  const r = await fetch(
    `${base}/api/v1/db/ui_actions?session_id=${encodeURIComponent(sessionId)}&limit=${encodeURIComponent(
      String(limit),
    )}&offset=${encodeURIComponent(String(offset))}`,
    { headers: daemonHeaders(auth) },
  );
  const j = await r.json();
  return DbUiActionsSchema.parse(j);
}

export async function apiGetDbSessions(
  base: string,
  auth?: ApiAuth,
  opts?: { limit?: number; offset?: number },
): Promise<DbSessionsResp> {
  const limit = typeof opts?.limit === "number" ? opts.limit : 50;
  const offset = typeof opts?.offset === "number" ? opts.offset : 0;
  const r = await fetch(
    `${base}/api/v1/db/sessions?limit=${encodeURIComponent(String(limit))}&offset=${encodeURIComponent(String(offset))}`,
    { headers: daemonHeaders(auth) },
  );
  const j = await r.json();
  return DbSessionsSchema.parse(j);
}

export async function apiGetDbMessages(
  base: string,
  sessionId: string,
  auth?: ApiAuth,
  opts?: { limit?: number; offset?: number; maxContentBytes?: number; maxMmBytes?: number },
): Promise<DbMessagesResp> {
  const limit = typeof opts?.limit === "number" ? opts.limit : 50;
  const offset = typeof opts?.offset === "number" ? opts.offset : 0;
  const maxContentBytes = typeof opts?.maxContentBytes === "number" ? opts.maxContentBytes : 8192;
  const maxMmBytes = typeof opts?.maxMmBytes === "number" ? opts.maxMmBytes : 1024 * 1024;
  const r = await fetch(
    `${base}/api/v1/db/messages?session_id=${encodeURIComponent(sessionId)}&limit=${encodeURIComponent(
      String(limit),
    )}&offset=${encodeURIComponent(String(offset))}&max_content_bytes=${encodeURIComponent(
      String(maxContentBytes),
    )}&max_mm_bytes=${encodeURIComponent(String(maxMmBytes))}`,
    { headers: daemonHeaders(auth) },
  );
  const j = await r.json();
  return DbMessagesSchema.parse(j);
}

export async function apiGetDbClientEvents(
  base: string,
  sessionId: string,
  auth?: ApiAuth,
  opts?: { limit?: number; offset?: number },
): Promise<DbClientEventsResp> {
  const limit = typeof opts?.limit === "number" ? opts.limit : 50;
  const offset = typeof opts?.offset === "number" ? opts.offset : 0;
  const r = await fetch(
    `${base}/api/v1/db/client_events?session_id=${encodeURIComponent(sessionId)}&limit=${encodeURIComponent(
      String(limit),
    )}&offset=${encodeURIComponent(String(offset))}`,
    { headers: daemonHeaders(auth) },
  );
  const j = await r.json();
  return DbClientEventsSchema.parse(j);
}
