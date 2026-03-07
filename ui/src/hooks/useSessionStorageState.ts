import React from "react";

function safeParse<T>(raw: string | null): T | undefined {
  if (!raw) return undefined;
  try {
    return JSON.parse(raw) as T;
  } catch {
    return undefined;
  }
}

export default function useSessionStorageState<T>(key: string, initialValue: T) {
  const [value, setValue] = React.useState<T>(() => {
    if (typeof window === "undefined") return initialValue;
    const parsed = safeParse<T>(window.sessionStorage.getItem(key));
    return parsed === undefined ? initialValue : parsed;
  });

  React.useEffect(() => {
    try {
      window.sessionStorage.setItem(key, JSON.stringify(value));
    } catch {
      // ignore quota / private mode errors
    }
  }, [key, value]);

  return [value, setValue] as const;
}
