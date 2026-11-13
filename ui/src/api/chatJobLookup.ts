import { daemonFetchInit, type ApiAuth } from "./auth";
export async function lookupChatJob(base: string, traceId: string, sessionId: string, auth?: ApiAuth) {
  const query = new URLSearchParams({ trace_id: traceId, session_id: sessionId });
  const response = await fetch(base + "/api/v1/job?" + query, daemonFetchInit(auth));
  const body = await response.json();
  if (!response.ok || body.ok !== true) throw new Error(body.error || "Job receipt lookup failed");
  if (body.trace_id !== traceId || body.session_id !== sessionId || body.durable_only !== true ||
      typeof body.job_id !== "string" || !body.job_id || typeof body.status !== "string")
    throw new Error("Job receipt identity mismatch");
  return body as { job_id: string; trace_id: string; session_id: string; status: string; durable_only: true };
}
