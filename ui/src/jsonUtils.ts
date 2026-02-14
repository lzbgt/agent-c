export function loadJson(raw: string): any {
  try {
    const v = JSON.parse(String(raw || ""));
    return v && typeof v === "object" ? v : null;
  } catch {
    return null;
  }
}
