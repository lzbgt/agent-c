export const addQueryParam = (params: URLSearchParams, key: string, value?: string | number | boolean) => {
  if (value === undefined || value === null) return;
  const s = typeof value === "string" ? value.trim() : String(value);
  if (!s) return;
  params.set(key, s);
};
