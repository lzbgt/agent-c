import React from "react";
import {
  apiBrokerEventsReplay,
  apiBrokerGetClientPrefs,
  apiBrokerPostClientPrefs,
  type ApiAuth,
  type BrokerEvent,
} from "../../api";
import useLocalStorageState from "../../hooks/useLocalStorageState";
import { extractTeamEventsCursorByScope, normalizeBrokerReplayEvents } from "./teamEventPrefs";
import { GUIDANCE_EVENT_TYPES, ORCHESTRATOR_EVENT_TYPES, TEAM_RUN_EVENT_TYPES } from "./teamRunUtils";
import type { BrokerEventRow, TeamCursorEntry } from "./types";

const TEAM_EVENTS_MAX = 200;
const TEAM_EVENTS_PREFS_KIND = "webui-team-events";
const TEAM_EVENTS_PREFS_VERSION = 1;

type UseBrokerTeamEventsStateArgs = {
  base: string;
  auth: ApiAuth;
  authKey: string;
  clientId: string;
  canQuery: boolean;
  teamIdTrimmed: string;
  quorumEvents?: BrokerEventRow[];
};

export default function useBrokerTeamEventsState({
  base,
  auth,
  authKey,
  clientId,
  canQuery,
  teamIdTrimmed,
  quorumEvents,
}: UseBrokerTeamEventsStateArgs) {
  const [teamReplayEvents, setTeamReplayEvents] = React.useState<BrokerEventRow[]>([]);
  const [orchestratorReplayEvents, setOrchestratorReplayEvents] = React.useState<BrokerEventRow[]>([]);
  const [teamReplayBusy, setTeamReplayBusy] = React.useState<boolean>(false);
  const [teamReplayError, setTeamReplayError] = React.useState<string | null>(null);
  const [teamReplayNote, setTeamReplayNote] = React.useState<string | null>(null);
  const [teamEventsCursorByTeam, setTeamEventsCursorByTeam] = useLocalStorageState<Record<string, number>>(
    "agentui.teamEventsCursorByTeam",
    {},
  );
  const teamEventsCursorRef = React.useRef<number>(0);
  const [teamEventsCursorByScope, setTeamEventsCursorByScope] = React.useState<Record<string, TeamCursorEntry>>({});
  const [teamEventsCursorStatus, setTeamEventsCursorStatus] = React.useState<"idle" | "loading" | "ready" | "error">(
    "idle",
  );
  const teamEventsCursorPersistRef = React.useRef<{
    timer: ReturnType<typeof setTimeout> | null;
    pending: Record<string, TeamCursorEntry> | null;
  }>({ timer: null, pending: null });

  const teamEventsCursor = teamIdTrimmed ? teamEventsCursorByTeam[teamIdTrimmed] || 0 : 0;
  const teamEventsScopeKey = React.useMemo(() => {
    if (!teamIdTrimmed) return "";
    const trimmedBase = String(base || "").trim();
    const key = String(authKey || "").trim();
    return `${trimmedBase}::${key}::${teamIdTrimmed}`;
  }, [authKey, base, teamIdTrimmed]);
  const teamPrefsClientId = React.useMemo(() => String(clientId || "webui"), [clientId]);
  const teamPrefsBase = React.useMemo(() => String(base || "").trim(), [base]);

  React.useEffect(() => {
    teamEventsCursorRef.current = teamEventsCursor;
  }, [teamEventsCursor]);

  const pushTeamEventsCursor = React.useCallback(
    async (nextMap: Record<string, TeamCursorEntry>) => {
      if (!teamPrefsBase || !teamPrefsClientId || !teamEventsScopeKey) return;
      const payload = {
        client_id: teamPrefsClientId,
        client_kind: TEAM_EVENTS_PREFS_KIND,
        prefs: {
          team_events_cursor: {
            version: TEAM_EVENTS_PREFS_VERSION,
            by_scope: nextMap,
          },
        },
      };
      const resp = await apiBrokerPostClientPrefs(teamPrefsBase, payload, auth);
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "team events prefs update failed");
      }
      setTeamEventsCursorStatus("ready");
    },
    [auth, teamEventsScopeKey, teamPrefsBase, teamPrefsClientId],
  );

  const scheduleTeamEventsCursorPersist = React.useCallback(
    (nextMap: Record<string, TeamCursorEntry>) => {
      if (!teamPrefsBase || !teamPrefsClientId || !teamEventsScopeKey) return;
      if (teamEventsCursorStatus === "error") return;
      teamEventsCursorPersistRef.current.pending = nextMap;
      if (teamEventsCursorPersistRef.current.timer) return;
      teamEventsCursorPersistRef.current.timer = setTimeout(() => {
        const pending = teamEventsCursorPersistRef.current.pending;
        teamEventsCursorPersistRef.current.pending = null;
        teamEventsCursorPersistRef.current.timer = null;
        if (!pending) return;
        pushTeamEventsCursor(pending).catch(() => {
          setTeamEventsCursorStatus("error");
        });
      }, 1500);
    },
    [pushTeamEventsCursor, teamEventsCursorStatus, teamEventsScopeKey, teamPrefsBase, teamPrefsClientId],
  );

  const loadTeamEventsCursor = React.useCallback(async () => {
    if (!canQuery || !teamPrefsBase || !teamPrefsClientId || !teamEventsScopeKey) return;
    setTeamEventsCursorStatus("loading");
    try {
      const resp = await apiBrokerGetClientPrefs(teamPrefsBase, teamPrefsClientId, TEAM_EVENTS_PREFS_KIND, auth);
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "team events prefs fetch failed");
      }
      const remoteMap = extractTeamEventsCursorByScope(resp.prefs);
      setTeamEventsCursorByScope(remoteMap);
      const remoteTs = remoteMap[teamEventsScopeKey]?.cursor_ts || 0;
      const localTs = teamEventsCursorRef.current || 0;
      const merged = Math.max(localTs, remoteTs);
      if (merged > localTs && teamIdTrimmed) {
        setTeamEventsCursorByTeam((prev) => ({ ...prev, [teamIdTrimmed]: merged }));
      }
      if (merged > remoteTs) {
        const nextMap = {
          ...remoteMap,
          [teamEventsScopeKey]: { cursor_ts: merged, updated_unix_ms: Date.now() },
        };
        setTeamEventsCursorByScope(nextMap);
        scheduleTeamEventsCursorPersist(nextMap);
      }
      setTeamEventsCursorStatus("ready");
    } catch {
      setTeamEventsCursorStatus("error");
    }
  }, [
    auth,
    canQuery,
    scheduleTeamEventsCursorPersist,
    setTeamEventsCursorByTeam,
    teamEventsScopeKey,
    teamIdTrimmed,
    teamPrefsBase,
    teamPrefsClientId,
  ]);

  const eventTeamId = React.useCallback((row?: BrokerEventRow | null) => {
    const payload = row?.payload;
    if (!payload || typeof payload !== "object") return "";
    return String((payload as { team_id?: unknown }).team_id || "");
  }, []);

  const isTeamEvent = React.useCallback(
    (row?: BrokerEventRow | null) => {
      if (!row) return false;
      const type = String(row.type || "");
      if (!TEAM_RUN_EVENT_TYPES.has(type)) return false;
      if (!teamIdTrimmed) return true;
      return eventTeamId(row) === teamIdTrimmed;
    },
    [eventTeamId, teamIdTrimmed],
  );

  const isGuidanceEvent = React.useCallback(
    (row?: BrokerEventRow | null) => {
      if (!row) return false;
      const type = String(row.type || "");
      if (!GUIDANCE_EVENT_TYPES.has(type)) return false;
      if (!teamIdTrimmed) return true;
      return eventTeamId(row) === teamIdTrimmed;
    },
    [eventTeamId, teamIdTrimmed],
  );

  const isOrchestratorEvent = React.useCallback(
    (row?: BrokerEventRow | null) => {
      if (!row) return false;
      const type = String(row.type || "");
      if (!ORCHESTRATOR_EVENT_TYPES.has(type)) return false;
      if (!teamIdTrimmed) return true;
      return eventTeamId(row) === teamIdTrimmed;
    },
    [eventTeamId, teamIdTrimmed],
  );

  const buildEventKey = (row: BrokerEventRow) =>
    row.event_id || `${row.type || ""}:${row.ts_unix_ms || 0}:${row.trace_id || ""}`;

  const mergeTeamEvents = React.useCallback(
    (live: BrokerEventRow[], replay: BrokerEventRow[]) => {
      const seen = new Set<string>();
      const out: BrokerEventRow[] = [];
      const push = (row: BrokerEventRow) => {
        const key = buildEventKey(row);
        if (seen.has(key)) return;
        seen.add(key);
        out.push(row);
      };
      for (const row of replay) {
        if (!isTeamEvent(row)) continue;
        push(row);
      }
      for (const row of live) {
        if (!isTeamEvent(row)) continue;
        push(row);
      }
      out.sort((a, b) => (a.ts_unix_ms || 0) - (b.ts_unix_ms || 0));
      return out.length > TEAM_EVENTS_MAX ? out.slice(out.length - TEAM_EVENTS_MAX) : out;
    },
    [isTeamEvent],
  );

  const mergeOrchestratorEvents = React.useCallback(
    (live: BrokerEventRow[], replay: BrokerEventRow[]) => {
      const seen = new Set<string>();
      const out: BrokerEventRow[] = [];
      const push = (row: BrokerEventRow) => {
        const key = buildEventKey(row);
        if (seen.has(key)) return;
        seen.add(key);
        out.push(row);
      };
      for (const row of replay) {
        if (!isOrchestratorEvent(row)) continue;
        push(row);
      }
      for (const row of live) {
        if (!isOrchestratorEvent(row)) continue;
        push(row);
      }
      out.sort((a, b) => (a.ts_unix_ms || 0) - (b.ts_unix_ms || 0));
      return out.length > TEAM_EVENTS_MAX ? out.slice(out.length - TEAM_EVENTS_MAX) : out;
    },
    [isOrchestratorEvent],
  );

  const mergeGuidanceEvents = React.useCallback(
    (live: BrokerEventRow[], replay: BrokerEventRow[]) => {
      const seen = new Set<string>();
      const out: BrokerEventRow[] = [];
      const push = (row: BrokerEventRow) => {
        const key = buildEventKey(row);
        if (seen.has(key)) return;
        seen.add(key);
        out.push(row);
      };
      for (const row of replay) {
        if (!isGuidanceEvent(row)) continue;
        push(row);
      }
      for (const row of live) {
        if (!isGuidanceEvent(row)) continue;
        push(row);
      }
      out.sort((a, b) => (a.ts_unix_ms || 0) - (b.ts_unix_ms || 0));
      return out.length > TEAM_EVENTS_MAX ? out.slice(out.length - TEAM_EVENTS_MAX) : out;
    },
    [isGuidanceEvent],
  );

  const liveEvents = Array.isArray(quorumEvents) ? quorumEvents : [];
  const mergedTeamEvents = React.useMemo(
    () => mergeTeamEvents(liveEvents, teamReplayEvents),
    [liveEvents, mergeTeamEvents, teamReplayEvents],
  );
  const mergedOrchestratorEvents = React.useMemo(
    () => mergeOrchestratorEvents(liveEvents, orchestratorReplayEvents),
    [liveEvents, mergeOrchestratorEvents, orchestratorReplayEvents],
  );
  const mergedGuidanceEvents = React.useMemo(
    () => mergeGuidanceEvents(liveEvents, teamReplayEvents),
    [liveEvents, mergeGuidanceEvents, teamReplayEvents],
  );

  React.useEffect(() => {
    void loadTeamEventsCursor();
  }, [loadTeamEventsCursor]);

  const loadTeamReplay = React.useCallback(async () => {
    if (!canQuery || !teamIdTrimmed) return;
    setTeamReplayBusy(true);
    setTeamReplayError(null);
    setTeamReplayNote(null);
    try {
      const resp = await apiBrokerEventsReplay(base, auth, {
        sinceTs: teamEventsCursorRef.current || 0,
        limit: TEAM_EVENTS_MAX,
        types: Array.from(new Set([...TEAM_RUN_EVENT_TYPES, ...ORCHESTRATOR_EVENT_TYPES, ...GUIDANCE_EVENT_TYPES])),
      });
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "team events replay failed");
      }
      const items = Array.isArray(resp.events) ? resp.events : [];
      const rows = normalizeBrokerReplayEvents(items as BrokerEvent[]);
      setTeamReplayEvents(rows.filter((row) => isTeamEvent(row)));
      setOrchestratorReplayEvents(rows.filter((row) => isOrchestratorEvent(row)));
      let nextCursor = teamEventsCursorRef.current || 0;
      if (typeof resp.next_since_ts === "number") {
        nextCursor = Math.max(nextCursor, resp.next_since_ts);
      }
      for (const row of rows) {
        if (row.ts_unix_ms && row.ts_unix_ms > nextCursor) nextCursor = row.ts_unix_ms;
      }
      if (teamIdTrimmed && nextCursor > (teamEventsCursorByTeam[teamIdTrimmed] || 0)) {
        setTeamEventsCursorByTeam((prev) => ({ ...prev, [teamIdTrimmed]: nextCursor }));
      }
      setTeamReplayNote(`replay +${rows.length}`);
    } catch (err) {
      setTeamReplayError(String(err));
    } finally {
      setTeamReplayBusy(false);
    }
  }, [auth, base, canQuery, isOrchestratorEvent, isTeamEvent, setTeamEventsCursorByTeam, teamEventsCursorByTeam, teamIdTrimmed]);

  React.useEffect(() => {
    if (!teamIdTrimmed) {
      setTeamReplayEvents([]);
      setTeamReplayError(null);
      setTeamReplayNote(null);
      return;
    }
    void loadTeamReplay();
  }, [loadTeamReplay, teamIdTrimmed]);

  React.useEffect(() => {
    if (!teamIdTrimmed) return;
    const rows = Array.isArray(quorumEvents) ? quorumEvents : [];
    let maxTs = teamEventsCursorRef.current || 0;
    for (const row of rows) {
      if (!isTeamEvent(row)) continue;
      const ts = row.ts_unix_ms || 0;
      if (ts > maxTs) maxTs = ts;
    }
    if (maxTs > (teamEventsCursorByTeam[teamIdTrimmed] || 0)) {
      setTeamEventsCursorByTeam((prev) => ({ ...prev, [teamIdTrimmed]: maxTs }));
    }
  }, [isTeamEvent, quorumEvents, setTeamEventsCursorByTeam, teamEventsCursorByTeam, teamIdTrimmed]);

  React.useEffect(() => {
    if (!teamEventsScopeKey || teamEventsCursor <= 0) return;
    const current = teamEventsCursorByScope[teamEventsScopeKey]?.cursor_ts || 0;
    if (teamEventsCursor <= current) return;
    const nextMap = {
      ...teamEventsCursorByScope,
      [teamEventsScopeKey]: { cursor_ts: teamEventsCursor, updated_unix_ms: Date.now() },
    };
    setTeamEventsCursorByScope(nextMap);
    scheduleTeamEventsCursorPersist(nextMap);
  }, [scheduleTeamEventsCursorPersist, teamEventsCursor, teamEventsCursorByScope, teamEventsScopeKey]);

  return {
    teamReplayBusy,
    teamReplayError,
    teamReplayNote,
    mergedTeamEvents,
    mergedOrchestratorEvents,
    mergedGuidanceEvents,
    loadTeamReplay,
  };
}
