import { daemonFetchInit, type ApiAuth } from "./auth";
export class ChatJobLookupError extends Error {
  constructor(message: string, public status: number) { super(message); this.name = "ChatJobLookupError"; }
}
export async function lookupChatJob(base: string, traceId: string, sessionId: string, auth?: ApiAuth, signal?: AbortSignal) {
  const query = new URLSearchParams({ trace_id: traceId, session_id: sessionId });
  const controller = new AbortController();
  const abort = () => controller.abort();
  if (signal?.aborted) controller.abort();
  signal?.addEventListener("abort", abort, { once: true });
  const deadline = setTimeout(abort, 5000);
  try {
    const response = await fetch(base + "/api/v1/job?" + query, daemonFetchInit(auth, { signal: controller.signal }));
    // Native metadata is small. Do not let a proxy response turn this recovery
    // path into an unbounded body download or expose provider request material.
    const reader = response.body?.getReader();
    if (!reader) throw new ChatJobLookupError("Missing job receipt body", 0);
    const chunks: Uint8Array[] = [];
    let size = 0;
    try {
      while (true) {
        const part = await reader.read();
        if (part.done) break;
        size += part.value.byteLength;
        if (size > 65536) throw new ChatJobLookupError("Job receipt exceeded its size limit", 0);
        chunks.push(part.value);
      }
    } finally {
      await reader.cancel().catch(() => {});
      reader.releaseLock();
    }
    const bytes = new Uint8Array(size);
    let offset = 0;
    for (const chunk of chunks) { bytes.set(chunk, offset); offset += chunk.byteLength; }
    let body;
    try { body = JSON.parse(new TextDecoder().decode(bytes)); }
    catch { throw new ChatJobLookupError("Invalid job receipt body", 0); }
    if (!response.ok) throw new ChatJobLookupError("Original job receipt is unavailable", response.status);
    if (body?.ok !== true || body.trace_id !== traceId || body.session_id !== sessionId || body.durable_only !== true ||
        typeof body.job_id !== "string" || body.job_id.length === 0 || body.job_id.length > 256 ||
        !["queued", "running", "done", "error", "cancelled", "interrupted"].includes(body.status))
      throw new ChatJobLookupError("Job receipt identity or state mismatch", 0);
    return body as { job_id: string; trace_id: string; session_id: string; status: string; durable_only: true };
  } finally {
    clearTimeout(deadline);
    signal?.removeEventListener("abort", abort);
  }
}
