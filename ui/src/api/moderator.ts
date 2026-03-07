import { daemonFetchInit, type ApiAuth } from "./auth";
import {
  ModeratorDirectiveReqSchema,
  type ModeratorDirectiveReq,
  ModeratorTaskReqSchema,
  type ModeratorTaskReq,
  ModeratorPostRespSchema,
  type ModeratorPostResp,
  ModeratorEventsRespSchema,
  type ModeratorEventsResp,
} from "./schemas/moderator";

export async function apiPostModeratorDirective(
  base: string,
  req: ModeratorDirectiveReq,
  auth?: ApiAuth,
): Promise<ModeratorPostResp> {
  const payload = ModeratorDirectiveReqSchema.parse(req);
  const r = await fetch(
    `${base}/api/v1/moderator/directive`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(payload) }, { "Content-Type": "application/json" }),
  );
  const j = await r.json();
  return ModeratorPostRespSchema.parse(j);
}

export async function apiPostModeratorTask(base: string, req: ModeratorTaskReq, auth?: ApiAuth): Promise<ModeratorPostResp> {
  const payload = ModeratorTaskReqSchema.parse(req);
  const r = await fetch(
    `${base}/api/v1/moderator/task`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(payload) }, { "Content-Type": "application/json" }),
  );
  const j = await r.json();
  return ModeratorPostRespSchema.parse(j);
}

export async function apiGetModeratorEvents(
  base: string,
  sessionId: string,
  auth?: ApiAuth,
  opts?: { maxBytes?: number; types?: string[] },
): Promise<ModeratorEventsResp> {
  const maxBytes = typeof opts?.maxBytes === "number" ? opts.maxBytes : 1024 * 1024;
  const types = Array.isArray(opts?.types) && opts?.types.length > 0 ? opts?.types.join(",") : "";
  const qs = new URLSearchParams({
    session_id: sessionId,
    max_bytes: String(maxBytes),
  });
  if (types) qs.set("types", types);
  const r = await fetch(`${base}/api/v1/moderator/events?${qs.toString()}`, daemonFetchInit(auth));
  const j = await r.json();
  return ModeratorEventsRespSchema.parse(j);
}
