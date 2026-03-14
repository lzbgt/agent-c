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

export function readRows(payload: any, key: string): any[] {
  return payload?.ok && Array.isArray(payload?.[key]) ? payload[key] : [];
}

export function shellRefOf(row: any): string {
  return String(row?.job_id || row?.job_ref || row?.alias || row?.id || "").trim();
}

export function serviceRefOf(row: any): string {
  return String(row?.job_id || row?.job_ref || row?.alias || row?.id || "").trim();
}

export function capabilityRefOf(row: any): string {
  return String(row?.name || row?.capability || row?.id || "").trim();
}

export function shellSummary(row: any): string {
  const ref = shellRefOf(row) || "shell";
  const label = String(row?.label || row?.intent || "").trim();
  const status = String(row?.status || row?.state || "").trim();
  return [ref, label, status].filter(Boolean).join(" · ");
}

export function serviceSummary(row: any): string {
  const ref = serviceRefOf(row) || "service";
  const label = String(row?.label || "").trim();
  const status = String(row?.status || row?.state || "").trim();
  return [ref, label, status].filter(Boolean).join(" · ");
}

export function capabilitySummary(row: any): string {
  const ref = capabilityRefOf(row) || "capability";
  const providers = Array.isArray(row?.providers) ? row.providers.length : 0;
  const consumers = Array.isArray(row?.consumers) ? row.consumers.length : 0;
  return `${ref}${providers || consumers ? ` · providers=${providers} consumers=${consumers}` : ""}`;
}

export function shellOutputText(payload: any): string {
  const candidates = [
    payload?.stdout,
    payload?.stderr,
    payload?.job?.stdout,
    payload?.job?.stderr,
    payload?.output,
    payload?.job?.output,
  ];
  for (const value of candidates) {
    if (typeof value === "string" && value.trim().length > 0) return value;
  }
  return "";
}
