import React from "react";
import {
  apiGetDbRun,
  apiRunAttestation,
  apiRunReplay,
  type ApiAuth,
  type DbRunResp,
  type RunAttestationResp,
  type RunReplayResp,
} from "../../api";
import useLocalStorageState from "../../hooks/useLocalStorageState";
import {
  MAX_DIFFS,
  artifactFingerprint,
  countByType,
  diffJson,
  diffJsonWithPath,
  extractUsage,
  extractUsageFromRunRow,
  normalizeArtifact,
  normalizeEvent,
  type DbDiffState,
  type ReplayDiffState,
} from "./runDiffUtils";

export type RunDiffPanelStateArgs = {
  baseUrl: string;
  auth: ApiAuth;
};

export type RunDiffSideState = {
  runId: string;
  setRunId: React.Dispatch<React.SetStateAction<string>>;
  loadBusy: boolean;
  error: string | null;
  replay: RunReplayResp | null;
  db: DbRunResp | null;
  attestation: RunAttestationResp | null;
  dbError: string | null;
  attestationError: string | null;
  evidenceBusy: boolean;
  loadReplay: () => Promise<void>;
  loadEvidence: () => Promise<void>;
};

export type RunDiffPanelState = {
  canQuery: boolean;
  baselineRunId: string;
  diffOnly: boolean;
  setDiffOnly: React.Dispatch<React.SetStateAction<boolean>>;
  setBaselineFromA: () => void;
  useBaselineForB: () => void;
  sideA: RunDiffSideState;
  sideB: RunDiffSideState;
  replayDiff: ReplayDiffState | null;
  dbDiff: DbDiffState | null;
  sameReplayHash: boolean;
};

export function useRunDiffPanelState(args: RunDiffPanelStateArgs): RunDiffPanelState {
  const base = String(args.baseUrl || "").trim().replace(/\/+$/, "");
  const canQuery = base.length > 0;
  const baselineKey = React.useMemo(() => `agentui.runDiffBaseline::${base}`, [base]);
  const [baselineRunId, setBaselineRunId] = useLocalStorageState<string>(baselineKey, "");

  const [runIdA, setRunIdA] = React.useState("");
  const [runIdB, setRunIdB] = React.useState("");
  const [loadBusyA, setLoadBusyA] = React.useState(false);
  const [loadBusyB, setLoadBusyB] = React.useState(false);
  const [errorA, setErrorA] = React.useState<string | null>(null);
  const [errorB, setErrorB] = React.useState<string | null>(null);
  const [replayA, setReplayA] = React.useState<RunReplayResp | null>(null);
  const [replayB, setReplayB] = React.useState<RunReplayResp | null>(null);
  const [dbA, setDbA] = React.useState<DbRunResp | null>(null);
  const [dbB, setDbB] = React.useState<DbRunResp | null>(null);
  const [attA, setAttA] = React.useState<RunAttestationResp | null>(null);
  const [attB, setAttB] = React.useState<RunAttestationResp | null>(null);
  const [dbErrA, setDbErrA] = React.useState<string | null>(null);
  const [dbErrB, setDbErrB] = React.useState<string | null>(null);
  const [attErrA, setAttErrA] = React.useState<string | null>(null);
  const [attErrB, setAttErrB] = React.useState<string | null>(null);
  const [evidenceBusyA, setEvidenceBusyA] = React.useState(false);
  const [evidenceBusyB, setEvidenceBusyB] = React.useState(false);
  const [diffOnly, setDiffOnly] = React.useState(true);

  const loadReplay = React.useCallback(
    async (
      runId: string,
      setBusy: React.Dispatch<React.SetStateAction<boolean>>,
      setErr: React.Dispatch<React.SetStateAction<string | null>>,
      setData: React.Dispatch<React.SetStateAction<RunReplayResp | null>>,
    ) => {
      const id = String(runId || "").trim();
      if (!id) {
        setErr("missing run_id");
        return;
      }
      if (!canQuery) {
        setErr("missing base URL");
        return;
      }
      setErr(null);
      setBusy(true);
      try {
        const resp = await apiRunReplay(base, id, args.auth);
        if (!resp.ok) {
          throw new Error(resp.error || resp.err || resp.code || "run replay failed");
        }
        setData(resp);
      } catch (err) {
        setErr(String(err));
      } finally {
        setBusy(false);
      }
    },
    [args.auth, base, canQuery],
  );

  const loadEvidence = React.useCallback(
    async (
      runId: string,
      setBusy: React.Dispatch<React.SetStateAction<boolean>>,
      setDb: React.Dispatch<React.SetStateAction<DbRunResp | null>>,
      setDbErr: React.Dispatch<React.SetStateAction<string | null>>,
      setAtt: React.Dispatch<React.SetStateAction<RunAttestationResp | null>>,
      setAttErr: React.Dispatch<React.SetStateAction<string | null>>,
    ) => {
      const id = String(runId || "").trim();
      if (!id) {
        setDbErr("missing run_id");
        setAttErr("missing run_id");
        return;
      }
      if (!canQuery) {
        setDbErr("missing base URL");
        setAttErr("missing base URL");
        return;
      }
      setBusy(true);
      setDbErr(null);
      setAttErr(null);
      try {
        const runIdNum = Number.parseInt(id, 10);
        const tasks: Array<Promise<void>> = [];
        if (!Number.isNaN(runIdNum)) {
          tasks.push(
            apiGetDbRun(base, runIdNum, args.auth, {
              includeEvents: true,
              includeArtifacts: true,
            })
              .then((resp) => {
                setDb(resp);
                if (!resp.ok) {
                  setDbErr(resp.error || resp.err || resp.code || "DB run lookup failed");
                }
              })
              .catch((err) => {
                setDbErr(String(err));
              }),
          );
        } else {
          setDbErr("run_id must be numeric for DB lookup");
        }
        tasks.push(
          apiRunAttestation(base, id, args.auth)
            .then((resp) => {
              setAtt(resp);
              if (!resp.ok) {
                setAttErr(resp.error || resp.err || resp.code || "attestation lookup failed");
              }
            })
            .catch((err) => {
              setAttErr(String(err));
            }),
        );
        await Promise.all(tasks);
      } finally {
        setBusy(false);
      }
    },
    [args.auth, base, canQuery],
  );

  const replayDiff = React.useMemo<ReplayDiffState | null>(() => {
    if (!replayA?.bundle || !replayB?.bundle) return null;
    return {
      requestDiffs: diffJson(replayA.bundle.request, replayB.bundle.request, MAX_DIFFS),
      responseDiffs: diffJson(replayA.bundle.response, replayB.bundle.response, MAX_DIFFS),
      toolDiffs: diffJson(replayA.bundle.tool_records, replayB.bundle.tool_records, MAX_DIFFS),
      usageA: extractUsage(replayA.bundle.response),
      usageB: extractUsage(replayB.bundle.response),
    };
  }, [replayA, replayB]);

  const dbDiff = React.useMemo<DbDiffState | null>(() => {
    if (!dbA?.run || !dbB?.run) return null;
    const eventsA = (dbA.events || []).map(normalizeEvent);
    const eventsB = (dbB.events || []).map(normalizeEvent);
    const eventDiffs = [] as DbDiffState["eventDiffs"];
    if (eventsA.length !== eventsB.length) {
      eventDiffs.push({ path: "events.length", a: eventsA.length, b: eventsB.length });
    }
    const minLen = Math.min(eventsA.length, eventsB.length);
    for (let i = 0; i < minLen && eventDiffs.length < MAX_DIFFS; i += 1) {
      const a = eventsA[i];
      const b = eventsB[i];
      if (a.type !== b.type) {
        eventDiffs.push({ path: `events[${i}].type`, a: a.type, b: b.type });
      }
      if (eventDiffs.length >= MAX_DIFFS) break;
      const dataDiffs = diffJsonWithPath(a.data, b.data, `events[${i}].data`, MAX_DIFFS - eventDiffs.length);
      if (dataDiffs.length > 0) eventDiffs.push(...dataDiffs);
    }
    const typeCountsA = countByType(eventsA);
    const typeCountsB = countByType(eventsB);
    const typeDiffs: DbDiffState["typeDiffs"] = [];
    const typeKeys = Array.from(new Set([...typeCountsA.keys(), ...typeCountsB.keys()])).sort();
    for (const key of typeKeys) {
      const a = typeCountsA.get(key) ?? 0;
      const b = typeCountsB.get(key) ?? 0;
      if (a !== b) typeDiffs.push({ type: key, a, b });
    }

    const artifactsA = (dbA.artifacts || []).map(normalizeArtifact);
    const artifactsB = (dbB.artifacts || []).map(normalizeArtifact);
    const mapWithKeys = (items: ReturnType<typeof normalizeArtifact>[]) => {
      const map = new Map<string, ReturnType<typeof normalizeArtifact>>();
      const counts = new Map<string, number>();
      items.forEach((item) => {
        const baseKey = item.path || String(item.id || "artifact");
        const next = (counts.get(baseKey) ?? 0) + 1;
        counts.set(baseKey, next);
        map.set(next > 1 ? `${baseKey}#${next}` : baseKey, item);
      });
      return map;
    };
    const mapA = mapWithKeys(artifactsA);
    const mapB = mapWithKeys(artifactsB);
    const keys = Array.from(new Set([...mapA.keys(), ...mapB.keys()])).sort();
    const added: DbDiffState["added"] = [];
    const removed: DbDiffState["removed"] = [];
    const changed: DbDiffState["changed"] = [];

    for (const key of keys) {
      const a = mapA.get(key);
      const b = mapB.get(key);
      if (a && !b) {
        removed.push({ key, item: a });
      } else if (!a && b) {
        added.push({ key, item: b });
      } else if (a && b && artifactFingerprint(a) !== artifactFingerprint(b)) {
        changed.push({
          key,
          a,
          b,
          diffs: diffJsonWithPath(a.artifact_json, b.artifact_json, `artifacts.${key}`, MAX_DIFFS),
        });
      }
    }

    return {
      eventsA,
      eventsB,
      eventDiffs,
      typeDiffs,
      added,
      removed,
      changed,
      usageDbA: extractUsageFromRunRow(dbA.run),
      usageDbB: extractUsageFromRunRow(dbB.run),
    };
  }, [dbA, dbB]);

  const sameReplayHash =
    replayA?.replay_sha256 && replayB?.replay_sha256 ? replayA.replay_sha256 === replayB.replay_sha256 : false;

  return {
    canQuery,
    baselineRunId,
    diffOnly,
    setDiffOnly,
    setBaselineFromA: () => setBaselineRunId(String(runIdA || "").trim()),
    useBaselineForB: () => setRunIdB(baselineRunId),
    sideA: {
      runId: runIdA,
      setRunId: setRunIdA,
      loadBusy: loadBusyA,
      error: errorA,
      replay: replayA,
      db: dbA,
      attestation: attA,
      dbError: dbErrA,
      attestationError: attErrA,
      evidenceBusy: evidenceBusyA,
      loadReplay: () => loadReplay(runIdA, setLoadBusyA, setErrorA, setReplayA),
      loadEvidence: () => loadEvidence(runIdA, setEvidenceBusyA, setDbA, setDbErrA, setAttA, setAttErrA),
    },
    sideB: {
      runId: runIdB,
      setRunId: setRunIdB,
      loadBusy: loadBusyB,
      error: errorB,
      replay: replayB,
      db: dbB,
      attestation: attB,
      dbError: dbErrB,
      attestationError: attErrB,
      evidenceBusy: evidenceBusyB,
      loadReplay: () => loadReplay(runIdB, setLoadBusyB, setErrorB, setReplayB),
      loadEvidence: () => loadEvidence(runIdB, setEvidenceBusyB, setDbB, setDbErrB, setAttB, setAttErrB),
    },
    replayDiff,
    dbDiff,
    sameReplayHash,
  };
}
