import React from "react";

function safeParse<T>(raw: string | null): T | undefined {
  if (!raw) return undefined;
  try {
    return JSON.parse(raw) as T;
  } catch {
    return undefined;
  }
}

export default function useLocalStorageState<T>(key: string, initialValue: T) {
  const read = (storageKey: string): T => {
    if (typeof window === "undefined") return initialValue;
    try {
      const parsed = safeParse<T>(window.localStorage.getItem(storageKey));
      return parsed === undefined ? initialValue : parsed;
    } catch { return initialValue; }
  };
  const [snapshot, setSnapshot] = React.useState(() => ({ key, value: read(key) }));
  // A key change selects different state. Do this before effects commit so an
  // old conversation's queue/draft cannot be written under the new scope.
  if (snapshot.key !== key) setSnapshot({ key, value: read(key) });
  const setValue = React.useCallback<React.Dispatch<React.SetStateAction<T>>>((update) => {
    setSnapshot(previous => {
      // Network callbacks retain their original setter. Never apply a late
      // update to whichever connection/session happens to be selected now.
      if (previous.key !== key) return previous;
      const value = typeof update === "function"
        ? (update as (previous: T) => T)(previous.value) : update;
      return Object.is(value, previous.value) ? previous : { key, value };
    });
  }, [key]);

  React.useEffect(() => {
    try {
      if (snapshot.key === key) window.localStorage.setItem(key, JSON.stringify(snapshot.value));
    } catch {
      // ignore quota / private mode errors
    }
  }, [key, snapshot]);

  return [snapshot.key === key ? snapshot.value : read(key), setValue] as const;
}

