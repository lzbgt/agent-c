import React from "react";
import {
  apiBrokerEventsReplay,
  apiBrokerGetClientPrefs,
  apiBrokerPostClientPrefs,
  daemonFetchInit,
  type ApiAuth,
} from "../../api";
import { readSseStream } from "../../sse";
import {
  extractBrokerEventsCursorByScope,
  normalizeBrokerEventRow,
  normalizeBrokerReplayEvents,
  type BrokerCursorByScope,
} from "./brokerPanelUtils";
import type { BrokerEventRow } from "./types";

const BROKER_EVENTS_MAX = 200;
const BROKER_EVENTS_PREFS_KIND = "webui-broker-events";
const BROKER_EVENTS_PREFS_VERSION = 1;

type UseBrokerEventsStateArgs = {
  base: string;
  auth: ApiAuth;
  authKey: string;
  clientId: string;
  open: boolean;
  canQuery: boolean;
};

export default function useBrokerEventsState({
  base,
  auth,
  authKey,
  clientId,
  open,
  canQuery,
}: UseBrokerEventsStateArgs) {
  const [brokerEvents, setBrokerEvents] = React.useState<BrokerEventRow[]>([]);
  const [brokerEventsError, setBrokerEventsError] = React.useState<string | null>(null);
  const [brokerEventsActive, setBrokerEventsActive] = React.useState<boolean>(true);
  const [brokerEventsConnected, setBrokerEventsConnected] = React.useState<boolean>(false);
  const [brokerEventsQuorumOnly, setBrokerEventsQuorumOnly] = React.useState<boolean>(true);
  const [brokerEventsReplayBusy, setBrokerEventsReplayBusy] = React.useState<boolean>(false);
  const [brokerEventsReplayError, setBrokerEventsReplayError] = React.useState<string | null>(null);
  const [brokerEventsReplayNote, setBrokerEventsReplayNote] = React.useState<string | null>(null);
  const [brokerEventsCursorTs, setBrokerEventsCursorTs] = React.useState<number>(0);
  const brokerEventsCursorRef = React.useRef<number>(0);
  const brokerEventsReplayKeyRef = React.useRef<string>("");
  const [brokerEventsCursorByScope, setBrokerEventsCursorByScope] = React.useState<BrokerCursorByScope>({});
  const [brokerEventsCursorStatus, setBrokerEventsCursorStatus] = React.useState<"idle" | "loading" | "ready" | "error">(
    "idle",
  );
  const brokerEventsLoadKeyRef = React.useRef<string>("");
  const brokerEventsCursorPersistRef = React.useRef<{
    timer: ReturnType<typeof setTimeout> | null;
    pending: BrokerCursorByScope | null;
  }>({ timer: null, pending: null });

  const brokerEventsCursorKey = React.useMemo(() => {
    if (!base || !authKey) return "";
    return `agentd:broker:eventsCursor:${base}:${authKey}`;
  }, [authKey, base]);
  const brokerEventsScopeKey = React.useMemo(() => {
    if (!base) return "";
    return `${base}::${String(authKey || "").trim()}`;
  }, [authKey, base]);
  const brokerPrefsClientId = React.useMemo(() => String(clientId || "webui"), [clientId]);
  const brokerPrefsBase = base;

  React.useEffect(() => {
    brokerEventsCursorRef.current = brokerEventsCursorTs;
  }, [brokerEventsCursorTs]);

  const pushBrokerEventsCursor = React.useCallback(
    async (nextMap: BrokerCursorByScope) => {
      if (!brokerPrefsBase || !brokerPrefsClientId || !brokerEventsScopeKey) return;
      const payload = {
        client_id: brokerPrefsClientId,
        client_kind: BROKER_EVENTS_PREFS_KIND,
        prefs: {
          broker_events_cursor: {
            version: BROKER_EVENTS_PREFS_VERSION,
            by_scope: nextMap,
          },
        },
      };
      const resp = await apiBrokerPostClientPrefs(brokerPrefsBase, payload, auth);
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "broker events prefs update failed");
      }
      setBrokerEventsCursorStatus("ready");
    },
    [auth, brokerEventsScopeKey, brokerPrefsBase, brokerPrefsClientId],
  );

  const scheduleBrokerEventsCursorPersist = React.useCallback(
    (nextMap: BrokerCursorByScope) => {
      if (!brokerPrefsBase || !brokerPrefsClientId || !brokerEventsScopeKey) return;
      if (brokerEventsCursorStatus === "error") return;
      brokerEventsCursorPersistRef.current.pending = nextMap;
      if (brokerEventsCursorPersistRef.current.timer) return;
      brokerEventsCursorPersistRef.current.timer = setTimeout(() => {
        const pending = brokerEventsCursorPersistRef.current.pending;
        brokerEventsCursorPersistRef.current.pending = null;
        brokerEventsCursorPersistRef.current.timer = null;
        if (!pending) return;
        pushBrokerEventsCursor(pending).catch(() => {
          setBrokerEventsCursorStatus("error");
        });
      }, 1500);
    },
    [brokerEventsCursorStatus, brokerEventsScopeKey, brokerPrefsBase, brokerPrefsClientId, pushBrokerEventsCursor],
  );

  const loadBrokerEventsCursor = React.useCallback(async () => {
    if (!canQuery || !brokerPrefsBase || !brokerPrefsClientId || !brokerEventsScopeKey) return;
    setBrokerEventsCursorStatus("loading");
    try {
      const resp = await apiBrokerGetClientPrefs(brokerPrefsBase, brokerPrefsClientId, BROKER_EVENTS_PREFS_KIND, auth);
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "broker events prefs fetch failed");
      }
      const remoteMap = extractBrokerEventsCursorByScope(resp.prefs);
      setBrokerEventsCursorByScope(remoteMap);
      const remoteTs = remoteMap[brokerEventsScopeKey]?.cursor_ts || 0;
      const localTs = brokerEventsCursorRef.current || 0;
      const merged = Math.max(localTs, remoteTs);
      if (merged > localTs) {
        setBrokerEventsCursorTs(merged);
      }
      if (merged > remoteTs) {
        const nextMap = {
          ...remoteMap,
          [brokerEventsScopeKey]: { cursor_ts: merged, updated_unix_ms: Date.now() },
        };
        setBrokerEventsCursorByScope(nextMap);
        scheduleBrokerEventsCursorPersist(nextMap);
      }
      setBrokerEventsCursorStatus("ready");
    } catch {
      setBrokerEventsCursorStatus("error");
    }
  }, [auth, brokerEventsScopeKey, brokerPrefsBase, brokerPrefsClientId, canQuery, scheduleBrokerEventsCursorPersist]);
  const loadBrokerEventsCursorRef = React.useRef(loadBrokerEventsCursor);

  React.useEffect(() => {
    setBrokerEvents([]);
    setBrokerEventsError(null);
    setBrokerEventsReplayError(null);
    setBrokerEventsReplayNote(null);
    setBrokerEventsCursorTs(0);
    brokerEventsReplayKeyRef.current = "";
    if (!brokerEventsCursorKey || typeof window === "undefined") return;
    try {
      const raw = window.localStorage.getItem(brokerEventsCursorKey);
      if (!raw) return;
      const parsed = JSON.parse(raw);
      const ts = typeof parsed?.ts === "number" ? parsed.ts : 0;
      if (ts > 0) setBrokerEventsCursorTs(ts);
    } catch {
      // ignore cache errors
    }
  }, [brokerEventsCursorKey]);

  React.useEffect(() => {
    loadBrokerEventsCursorRef.current = loadBrokerEventsCursor;
  }, [loadBrokerEventsCursor]);

  React.useEffect(() => {
    if (!canQuery || !brokerPrefsBase || !brokerPrefsClientId || !authKey) return;
    const nextKey = `${brokerPrefsBase}::${brokerPrefsClientId}::${authKey}`;
    if (brokerEventsLoadKeyRef.current === nextKey) return;
    brokerEventsLoadKeyRef.current = nextKey;
    void loadBrokerEventsCursorRef.current();
  }, [authKey, brokerPrefsBase, brokerPrefsClientId, canQuery]);

  React.useEffect(() => {
    if (canQuery) return;
    brokerEventsLoadKeyRef.current = "";
  }, [canQuery]);

  React.useEffect(() => {
    if (!brokerEventsCursorKey || typeof window === "undefined") return;
    if (brokerEventsCursorTs <= 0) return;
    try {
      window.localStorage.setItem(
        brokerEventsCursorKey,
        JSON.stringify({ ts: brokerEventsCursorTs, updated: Date.now() }),
      );
    } catch {
      // ignore cache errors
    }
  }, [brokerEventsCursorKey, brokerEventsCursorTs]);

  React.useEffect(() => {
    if (!brokerEventsScopeKey || brokerEventsCursorTs <= 0) return;
    const current = brokerEventsCursorByScope[brokerEventsScopeKey]?.cursor_ts || 0;
    if (brokerEventsCursorTs <= current) return;
    const nextMap = {
      ...brokerEventsCursorByScope,
      [brokerEventsScopeKey]: { cursor_ts: brokerEventsCursorTs, updated_unix_ms: Date.now() },
    };
    setBrokerEventsCursorByScope(nextMap);
    scheduleBrokerEventsCursorPersist(nextMap);
  }, [brokerEventsCursorByScope, brokerEventsCursorTs, brokerEventsScopeKey, scheduleBrokerEventsCursorPersist]);

  const appendBrokerEvents = React.useCallback((rows: BrokerEventRow[]) => {
    if (rows.length === 0) return;
    setBrokerEvents((prev) => {
      const seen = new Set<string>();
      for (const ev of prev) {
        const key = ev?.event_id || `${ev?.type || ""}:${ev?.ts_unix_ms || 0}:${ev?.trace_id || ""}`;
        seen.add(key);
      }
      const next = prev.slice();
      for (const row of rows) {
        const key = row?.event_id || `${row?.type || ""}:${row?.ts_unix_ms || 0}:${row?.trace_id || ""}`;
        if (seen.has(key)) continue;
        seen.add(key);
        next.push(row);
      }
      return next.length <= BROKER_EVENTS_MAX ? next : next.slice(next.length - BROKER_EVENTS_MAX);
    });
  }, []);

  const updateBrokerEventsCursor = React.useCallback((rowTs?: number, nextSince?: number) => {
    const current = brokerEventsCursorRef.current || 0;
    const candidate = Math.max(current, rowTs || 0, nextSince || 0);
    if (candidate > current) {
      setBrokerEventsCursorTs(candidate);
    }
  }, []);

  const loadBrokerEventsReplay = React.useCallback(async () => {
    if (!canQuery || !base) return;
    setBrokerEventsReplayBusy(true);
    setBrokerEventsReplayError(null);
    try {
      const sinceTs = brokerEventsCursorRef.current || 0;
      const resp = await apiBrokerEventsReplay(base, auth, {
        sinceTs,
        limit: BROKER_EVENTS_MAX,
      });
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "events replay failed");
      }
      const rows = normalizeBrokerReplayEvents(resp.events);
      appendBrokerEvents(rows);
      if (typeof resp.next_since_ts === "number") {
        updateBrokerEventsCursor(undefined, resp.next_since_ts);
      } else {
        for (const row of rows) {
          updateBrokerEventsCursor(row.ts_unix_ms);
        }
      }
      setBrokerEventsReplayNote(`replay +${rows.length}`);
    } catch (err) {
      setBrokerEventsReplayError(String(err));
    } finally {
      setBrokerEventsReplayBusy(false);
    }
  }, [appendBrokerEvents, auth, base, canQuery, updateBrokerEventsCursor]);

  React.useEffect(() => {
    if (!open || !canQuery || !brokerEventsActive) {
      setBrokerEventsConnected(false);
      return;
    }
    const controller = new AbortController();
    const run = async () => {
      setBrokerEventsError(null);
      setBrokerEventsConnected(false);
      try {
        const resp = await fetch(`${base}/v1/events`, daemonFetchInit(auth, { signal: controller.signal }));
        if (!resp.ok) {
          throw new Error(`broker events failed (${resp.status})`);
        }
        setBrokerEventsConnected(true);
        await readSseStream(resp, (ev) => {
          if (controller.signal.aborted) return;
          let parsed: unknown = null;
          try {
            parsed = ev.data ? JSON.parse(ev.data) : null;
          } catch {
            parsed = null;
          }
          const row = normalizeBrokerEventRow(parsed, ev.event);
          if (!row) return;
          appendBrokerEvents([row]);
          updateBrokerEventsCursor(row.ts_unix_ms);
        });
      } catch (err) {
        if (controller.signal.aborted) return;
        setBrokerEventsConnected(false);
        setBrokerEventsError(String(err));
      }
    };
    void run();
    return () => controller.abort();
  }, [appendBrokerEvents, auth, base, brokerEventsActive, canQuery, open, updateBrokerEventsCursor]);

  React.useEffect(() => {
    if (!open || !canQuery || !brokerEventsActive || !base) return;
    const key = `${base}::${authKey}:${brokerEventsActive ? "on" : "off"}`;
    if (brokerEventsReplayKeyRef.current === key) return;
    brokerEventsReplayKeyRef.current = key;
    void loadBrokerEventsReplay();
  }, [authKey, base, brokerEventsActive, canQuery, loadBrokerEventsReplay, open]);

  const brokerEventRows = React.useMemo(() => {
    const rows = brokerEventsQuorumOnly
      ? brokerEvents.filter((ev) => String(ev?.type || "").startsWith("team_quorum"))
      : brokerEvents;
    return rows.slice().reverse();
  }, [brokerEvents, brokerEventsQuorumOnly]);

  const clearBrokerEvents = React.useCallback(() => {
    setBrokerEvents([]);
  }, []);

  return {
    brokerEvents,
    brokerEventsError,
    brokerEventsActive,
    setBrokerEventsActive,
    brokerEventsConnected,
    brokerEventsQuorumOnly,
    setBrokerEventsQuorumOnly,
    brokerEventsReplayBusy,
    brokerEventsReplayError,
    brokerEventsReplayNote,
    brokerEventRows,
    loadBrokerEventsReplay,
    clearBrokerEvents,
  };
}
