export type UnknownRecord = Record<string, unknown>;

export const isUnknownRecord = (value: unknown): value is UnknownRecord =>
  Boolean(value) && typeof value === "object" && !Array.isArray(value);

export const asUnknownRecord = (value: unknown): UnknownRecord | null => (isUnknownRecord(value) ? value : null);

export const parseUnknownRecordJson = (raw: string, fieldName: string): UnknownRecord => {
  const parsed: unknown = JSON.parse(raw);
  if (!isUnknownRecord(parsed)) {
    throw new Error(`${fieldName} must be a JSON object`);
  }
  return parsed;
};

export const asStringList = (value: unknown): string[] =>
  Array.isArray(value) ? value.map((item) => String(item).trim()).filter(Boolean) : [];

export const asFiniteNumber = (value: unknown): number | undefined => {
  if (typeof value === "number" && Number.isFinite(value)) return value;
  if (typeof value === "string" && value.trim()) {
    const parsed = Number(value);
    if (Number.isFinite(parsed)) return parsed;
  }
  return undefined;
};
