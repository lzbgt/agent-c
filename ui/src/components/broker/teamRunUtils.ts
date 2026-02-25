export const TEAM_RUN_EVENT_TYPES = new Set([
  "team_run_created",
  "team_run_status",
  "team_runtime_members_updated",
  "team_quorum_request",
  "team_quorum_result",
]);

export const fmtTs = (ms?: number | null) => {
  if (!ms || !Number.isFinite(ms)) return "";
  try {
    return new Date(ms).toLocaleString();
  } catch {
    return String(ms);
  }
};

export const fmtSummary = (summary?: any) => {
  if (!summary || typeof summary !== "object") return "";
  const parts: string[] = [];
  const pushIf = (key: string, label: string) => {
    const val = summary?.[key];
    if (typeof val === "number") parts.push(`${label} ${val}`);
  };
  pushIf("total", "total");
  pushIf("queued", "queued");
  pushIf("running", "running");
  pushIf("done", "done");
  pushIf("error", "error");
  pushIf("cancelled", "cancelled");
  pushIf("interrupted", "interrupted");
  pushIf("unknown", "unknown");
  pushIf("ok", "ok");
  pushIf("failed", "failed");
  pushIf("dispatch_errors", "dispatch_errors");
  return parts.join(" · ");
};

export const parseCsvList = (raw?: string | null) => {
  if (!raw) return [];
  return String(raw)
    .split(",")
    .map((item) => item.trim())
    .filter(Boolean);
};

export const normalizeRoleInstructionMap = (raw: any): Record<string, string> => {
  if (!raw || typeof raw !== "object" || Array.isArray(raw)) return {};
  const out: Record<string, string> = {};
  for (const [key, value] of Object.entries(raw)) {
    const role = String(key || "").trim().toLowerCase();
    if (!role) continue;
    let instr = "";
    if (typeof value === "string") instr = value;
    else if (value && typeof value === "object" && typeof (value as any).instruction === "string") {
      instr = String((value as any).instruction);
    }
    instr = instr.trim();
    if (instr) out[role] = instr;
  }
  return out;
};

export const normalizeRolePromptMode = (raw: any): string => {
  if (typeof raw !== "string") return "prepend";
  const mode = raw.trim().toLowerCase();
  if (mode === "append" || mode === "replace") return mode;
  return "prepend";
};
