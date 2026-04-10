import type { SessionOperatorResp } from "../../api";
import { isUnknownRecord, safeObject, type UnknownRecord } from "../../jsonUtils";

export type SessionOperatorRow = UnknownRecord;

export type SessionOperatorMutation = {
  isPending: boolean;
  isError: boolean;
  error: unknown;
  mutateAsync: (...args: unknown[]) => Promise<unknown>;
};

export function jsonText(value: unknown): string {
  try {
    return JSON.stringify(value ?? {}, null, 2);
  } catch {
    return String(value ?? "");
  }
}

export function parsePositiveInt(value: string): number | undefined {
  const n = Number(value);
  return Number.isFinite(n) && n > 0 ? Math.floor(n) : undefined;
}

export function readRows(payload: SessionOperatorResp | undefined, key: string): SessionOperatorRow[] {
  if (payload?.ok !== true) return [];
  const rows = safeObject(payload)[key];
  return Array.isArray(rows) ? rows.filter(isUnknownRecord) : [];
}

export function shellRefOf(row: unknown): string {
  const record = safeObject(row);
  return String(record.job_id || record.job_ref || record.alias || record.id || "").trim();
}

export function serviceRefOf(row: unknown): string {
  const record = safeObject(row);
  return String(record.job_id || record.job_ref || record.alias || record.id || "").trim();
}

export function capabilityRefOf(row: unknown): string {
  const record = safeObject(row);
  return String(record.name || record.capability || record.id || "").trim();
}

export function shellSummary(row: unknown): string {
  const record = safeObject(row);
  const ref = shellRefOf(row) || "shell";
  const label = String(record.label || record.intent || "").trim();
  const status = String(record.status || record.state || "").trim();
  return [ref, label, status].filter(Boolean).join(" · ");
}

export function serviceSummary(row: unknown): string {
  const record = safeObject(row);
  const ref = serviceRefOf(row) || "service";
  const label = String(record.label || "").trim();
  const status = String(record.status || record.state || "").trim();
  return [ref, label, status].filter(Boolean).join(" · ");
}

export function capabilitySummary(row: unknown): string {
  const record = safeObject(row);
  const ref = capabilityRefOf(row) || "capability";
  const providers = Array.isArray(record.providers) ? record.providers.length : 0;
  const consumers = Array.isArray(record.consumers) ? record.consumers.length : 0;
  return `${ref}${providers || consumers ? ` · providers=${providers} consumers=${consumers}` : ""}`;
}

export function shellOutputText(payload: unknown): string {
  const record = safeObject(payload);
  const job = safeObject(record.job);
  const candidates = [
    record.stdout,
    record.stderr,
    job.stdout,
    job.stderr,
    record.output,
    job.output,
  ];
  for (const value of candidates) {
    if (typeof value === "string" && value.trim().length > 0) return value;
  }
  return "";
}
