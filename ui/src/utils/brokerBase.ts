export const brokerBaseFromProxy = (baseUrl: string): string => {
  const trimmed = String(baseUrl || "").trim().replace(/\/+$/, "");
  const marker = "/v1/agents/";
  const idx = trimmed.indexOf(marker);
  if (idx >= 0) return trimmed.slice(0, idx);
  return trimmed;
};
