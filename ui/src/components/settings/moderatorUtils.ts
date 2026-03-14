import type { ModeratorEvent } from "../../api";

export type JsonDiffEntry = { path: string; a: unknown; b: unknown };

function truncateText(value: string, maxLen: number) {
  if (value.length <= maxLen) return value;
  return `${value.slice(0, Math.max(0, maxLen - 1))}…`;
}

export function formatDiffValue(value: unknown) {
  if (value === undefined) return "(undefined)";
  if (value === null) return "null";
  if (typeof value === "string") return truncateText(JSON.stringify(value), 200);
  try {
    return truncateText(JSON.stringify(value), 200);
  } catch {
    return truncateText(String(value), 200);
  }
}

function isPlainObject(value: unknown): value is Record<string, unknown> {
  return !!value && typeof value === "object" && !Array.isArray(value);
}

export function collectJsonDiffs(a: unknown, b: unknown, path: string, out: JsonDiffEntry[], maxDiffs: number) {
  if (out.length >= maxDiffs) return;
  if (a === b) return;
  const aIsArr = Array.isArray(a);
  const bIsArr = Array.isArray(b);
  if (aIsArr || bIsArr) {
    if (!aIsArr || !bIsArr) {
      out.push({ path: path || "<root>", a, b });
      return;
    }
    const max = Math.max(a.length, b.length);
    for (let i = 0; i < max; i += 1) {
      collectJsonDiffs(a[i], b[i], `${path}[${i}]`, out, maxDiffs);
      if (out.length >= maxDiffs) return;
    }
    return;
  }
  if (isPlainObject(a) && isPlainObject(b)) {
    const keys = new Set<string>([...Object.keys(a), ...Object.keys(b)]);
    for (const key of keys) {
      const nextPath = path ? `${path}.${key}` : key;
      collectJsonDiffs(a[key], b[key], nextPath, out, maxDiffs);
      if (out.length >= maxDiffs) return;
    }
    return;
  }
  out.push({ path: path || "<root>", a, b });
}

export function formatModeratorEventSummary(event: ModeratorEvent) {
  const type = typeof event?.type === "string" ? event.type : "";
  const data = event?.data && typeof event.data === "object" ? (event.data as any) : {};
  if (type === "moderator_directive") {
    const directive = typeof data?.directive === "string" ? data.directive : "";
    return directive || "(directive)";
  }
  if (type === "moderator_task_published") {
    const task = data?.task && typeof data.task === "object" ? data.task : {};
    const title = typeof task?.title === "string" ? task.title : "";
    return title || "(task)";
  }
  return "";
}
