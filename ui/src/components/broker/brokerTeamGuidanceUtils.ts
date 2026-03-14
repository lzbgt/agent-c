import type { BrokerGuidanceEvent } from "../../api";

export const normalizeGuidanceList = (rows?: BrokerGuidanceEvent[]) =>
  Array.isArray(rows) ? rows.filter((row) => row && typeof row === "object") : [];

export const formatDuration = (ms?: number | null) => {
  if (!ms || !Number.isFinite(ms)) return "";
  if (ms < 1000) return `${Math.round(ms)}ms`;
  const sec = ms / 1000;
  if (sec < 60) return `${sec.toFixed(1)}s`;
  const min = sec / 60;
  if (min < 60) return `${min.toFixed(1)}m`;
  const hr = min / 60;
  return `${hr.toFixed(1)}h`;
};
