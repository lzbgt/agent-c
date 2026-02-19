import { daemonHeaders, type ApiAuth } from "./auth";
import { OpenRouterModelsRespSchema, type OpenRouterModelsResp, ToolDefsRespSchema, type ToolDefsResp } from "./schemas/tools";

export async function apiGetTools(
  base: string,
  auth?: ApiAuth,
  opts?: { tools?: "host" | "basic" | "none"; yolo?: boolean; hostPolicy?: "full" | "readonly"; sessionId?: string },
): Promise<ToolDefsResp> {
  const q = new URLSearchParams();
  if (opts?.tools) q.set("tools", opts.tools);
  if (typeof opts?.yolo === "boolean") q.set("yolo", opts.yolo ? "1" : "0");
  if (opts?.hostPolicy) q.set("host_policy", opts.hostPolicy);
  if (typeof opts?.sessionId === "string" && opts.sessionId.length > 0) q.set("session_id", opts.sessionId);
  const r = await fetch(`${base}/api/v1/tools?${q.toString()}`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return ToolDefsRespSchema.parse(j);
}

export async function apiGetOpenRouterModels(
  base: string,
  opts: {
    daemonAuth?: ApiAuth;
    apiKey?: string;
    openrouterBaseUrl?: string;
    minTotal?: number;
    maxTotal?: number;
    requireMultimodalInput?: boolean;
    requireTools?: boolean;
    includeFree?: boolean;
    limit?: number;
    refresh?: boolean;
  },
): Promise<OpenRouterModelsResp> {
  const q = new URLSearchParams();
  if (opts.openrouterBaseUrl) q.set("base_url", opts.openrouterBaseUrl);
  if (typeof opts.minTotal === "number") q.set("min_total", String(opts.minTotal));
  if (typeof opts.maxTotal === "number") q.set("max_total", String(opts.maxTotal));
  if (typeof opts.requireMultimodalInput === "boolean")
    q.set("require_multimodal_input", opts.requireMultimodalInput ? "1" : "0");
  if (typeof opts.requireTools === "boolean") q.set("require_tools", opts.requireTools ? "1" : "0");
  if (typeof opts.includeFree === "boolean") q.set("include_free", opts.includeFree ? "1" : "0");
  if (typeof opts.limit === "number") q.set("limit", String(opts.limit));
  if (opts.refresh) q.set("refresh", "1");

  const headers: Record<string, string> = daemonHeaders(opts.daemonAuth);
  if (opts.apiKey && opts.apiKey.trim().length > 0) {
    headers["X-OpenRouter-Key"] = opts.apiKey.trim();
  }
  const r = await fetch(`${base}/api/v1/openrouter/models?${q.toString()}`, { headers });
  const j = await r.json();
  return OpenRouterModelsRespSchema.parse(j);
}
