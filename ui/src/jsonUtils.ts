export type UnknownRecord = Record<string, unknown>;

export function safeJsonParse(raw: string): unknown | null {
  try {
    return JSON.parse(String(raw || ""));
  } catch {
    return null;
  }
}

export function isUnknownRecord(value: unknown): value is UnknownRecord {
  return !!value && typeof value === "object" && !Array.isArray(value);
}

export function safeObject(value: unknown): UnknownRecord {
  return isUnknownRecord(value) ? value : {};
}

export function normalizeEventData(data: unknown): unknown {
  if (typeof data === "string") {
    return safeJsonParse(data) ?? data;
  }
  return data ?? {};
}

export function prettyJsonOrRaw(raw: string): string {
  const parsed = safeJsonParse(raw);
  if (parsed === null) return raw;
  return JSON.stringify(parsed, null, 2);
}

export function safeTrunc(value: string, max: number): string {
  const text = String(value ?? "");
  if (text.length <= max) return text;
  return text.slice(0, Math.max(0, max - 1)) + "…";
}

export function loadJson(raw: string): UnknownRecord | null {
  const value = safeJsonParse(raw);
  return isUnknownRecord(value) ? value : null;
}
