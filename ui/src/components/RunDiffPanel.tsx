import React from "react";
import {
  apiGetDbRun,
  apiRunAttestation,
  apiRunReplay,
  type ApiAuth,
  type DbRunResp,
  type RunAttestationResp,
  type RunReplayResp,
} from "../api";
import useLocalStorageState from "../hooks/useLocalStorageState";
import FieldLabel from "./FieldLabel";

export type RunDiffPanelProps = {
  open: boolean;
  onToggle: (open: boolean) => void;
  baseUrl: string;
  auth: ApiAuth;
};

type JsonDiffEntry = { path: string; a: unknown; b: unknown };
const MAX_DIFFS = 200;

const stringifyJson = (value: unknown) => {
  try {
    return JSON.stringify(value, null, 2);
  } catch {
    return String(value ?? "");
  }
};

const parseJsonLoose = (value: unknown) => {
  if (typeof value === "string") {
    try {
      return JSON.parse(value);
    } catch {
      return value;
    }
  }
  return value;
};

const formatDiffValue = (value: unknown) => {
  if (value === null) return "null";
  if (value === undefined) return "undefined";
  if (typeof value === "string") return value.length > 120 ? `${value.slice(0, 120)}…` : value;
  if (typeof value === "number" || typeof value === "boolean") return String(value);
  try {
    const raw = JSON.stringify(value);
    if (raw.length > 160) return `${raw.slice(0, 160)}…`;
    return raw;
  } catch {
    return String(value);
  }
};

const collectJsonDiffs = (
  a: unknown,
  b: unknown,
  path: string,
  out: JsonDiffEntry[],
  maxDiffs: number,
  depth: number,
  maxDepth: number,
  maxArrayItems: number,
) => {
  if (out.length >= maxDiffs) return;
  if (a === b) return;
  if (depth >= maxDepth) {
    out.push({ path, a, b });
    return;
  }
  if (Array.isArray(a) && Array.isArray(b)) {
    if (a.length !== b.length) {
      out.push({ path: path ? `${path}.length` : "length", a: a.length, b: b.length });
    }
    const limit = Math.min(a.length, b.length, maxArrayItems);
    for (let i = 0; i < limit; i += 1) {
      collectJsonDiffs(a[i], b[i], `${path}[${i}]`, out, maxDiffs, depth + 1, maxDepth, maxArrayItems);
      if (out.length >= maxDiffs) return;
    }
    if (a.length > maxArrayItems || b.length > maxArrayItems) {
      out.push({ path: path ? `${path}[truncated]` : "[truncated]", a: a.length, b: b.length });
    }
    return;
  }
  if (a && b && typeof a === "object" && typeof b === "object") {
    const keys = new Set<string>([...Object.keys(a as Record<string, unknown>), ...Object.keys(b as Record<string, unknown>)]);
    const sorted = Array.from(keys).sort();
    for (const key of sorted) {
      const nextPath = path ? `${path}.${key}` : key;
      collectJsonDiffs(
        (a as Record<string, unknown>)[key],
        (b as Record<string, unknown>)[key],
        nextPath,
        out,
        maxDiffs,
        depth + 1,
        maxDepth,
        maxArrayItems,
      );
      if (out.length >= maxDiffs) return;
    }
    return;
  }
  out.push({ path, a, b });
};

const diffJson = (a: unknown, b: unknown, maxDiffs = 200) => {
  const out: JsonDiffEntry[] = [];
  collectJsonDiffs(a, b, "", out, maxDiffs, 0, 12, 50);
  return out;
};

const diffJsonWithPath = (a: unknown, b: unknown, path: string, maxDiffs = 200) => {
  const out: JsonDiffEntry[] = [];
  collectJsonDiffs(a, b, path, out, maxDiffs, 0, 12, 50);
  return out;
};

const extractUsage = (resp: any) => {
  if (!resp || typeof resp !== "object") return null;
  const usage = resp.usage && typeof resp.usage === "object" ? resp.usage : {};
  const totalTokens = usage.total_tokens ?? resp.total_tokens;
  const promptTokens = usage.prompt_tokens ?? resp.prompt_tokens;
  const completionTokens = usage.completion_tokens ?? resp.completion_tokens;
  const totalCost = resp.total_cost ?? resp.cost ?? usage.total_cost ?? usage.cost;
  const out: Record<string, number> = {};
  if (typeof totalTokens === "number") out.total_tokens = totalTokens;
  if (typeof promptTokens === "number") out.prompt_tokens = promptTokens;
  if (typeof completionTokens === "number") out.completion_tokens = completionTokens;
  if (typeof totalCost === "number") out.total_cost = totalCost;
  return Object.keys(out).length > 0 ? out : null;
};

const extractUsageFromRunRow = (run: any) => {
  if (!run || typeof run !== "object") return null;
  const response = parseJsonLoose(run.response_json ?? run.response);
  return extractUsage(response);
};

const formatUsage = (usage?: Record<string, number> | null) => {
  if (!usage) return "";
  const parts: string[] = [];
  if (typeof usage.total_tokens === "number") parts.push(`tokens ${usage.total_tokens}`);
  if (typeof usage.prompt_tokens === "number") parts.push(`prompt ${usage.prompt_tokens}`);
  if (typeof usage.completion_tokens === "number") parts.push(`completion ${usage.completion_tokens}`);
  if (typeof usage.total_cost === "number") parts.push(`cost ${usage.total_cost.toFixed(4)}`);
  return parts.join(" · ");
};

const formatUsageDelta = (a?: Record<string, number> | null, b?: Record<string, number> | null) => {
  if (!a || !b) return "";
  const keys = Array.from(new Set([...Object.keys(a), ...Object.keys(b)])).sort();
  const parts: string[] = [];
  for (const key of keys) {
    const av = a[key];
    const bv = b[key];
    if (typeof av !== "number" || typeof bv !== "number") continue;
    const delta = bv - av;
    const sign = delta > 0 ? "+" : delta < 0 ? "-" : "";
    const magnitude = Math.abs(delta);
    parts.push(`${key} ${sign}${magnitude % 1 === 0 ? magnitude : magnitude.toFixed(4)}`);
  }
  return parts.join(" · ");
};

const normalizeEvent = (row: any) => {
  const data = parseJsonLoose(row?.data_json ?? row?.data);
  return {
    id: row?.event_id ?? row?.id ?? null,
    ts_unix_ms: row?.ts_unix_ms ?? row?.ts ?? null,
    type: row?.type ?? "",
    data,
  };
};

const countByType = (events: Array<{ type: string }>) => {
  const counts = new Map<string, number>();
  for (const ev of events) {
    const key = String(ev.type || "unknown");
    counts.set(key, (counts.get(key) ?? 0) + 1);
  }
  return counts;
};

const normalizeArtifact = (row: any) => {
  const artifactJson = parseJsonLoose(row?.artifact_json ?? row?.artifact ?? row);
  return {
    id: row?.id ?? null,
    path: row?.path ?? artifactJson?.path ?? "",
    kind: row?.kind ?? artifactJson?.kind ?? "",
    mime: row?.mime ?? artifactJson?.mime ?? "",
    title: row?.title ?? artifactJson?.title ?? "",
    tool_call_id: row?.tool_call_id ?? artifactJson?.tool_call_id ?? "",
    autoplay: row?.autoplay ?? artifactJson?.autoplay,
    repeat: row?.repeat ?? artifactJson?.repeat,
    artifact_json: artifactJson,
  };
};

const artifactFingerprint = (artifact: ReturnType<typeof normalizeArtifact>) =>
  stringifyJson({
    path: artifact.path,
    kind: artifact.kind,
    mime: artifact.mime,
    title: artifact.title,
    tool_call_id: artifact.tool_call_id,
    autoplay: artifact.autoplay,
    repeat: artifact.repeat,
    artifact_json: artifact.artifact_json,
  });

const formatArtifactSummary = (artifact?: ReturnType<typeof normalizeArtifact> | null) => {
  if (!artifact) return "";
  const parts = [artifact.path || "(no path)"];
  if (artifact.kind) parts.push(artifact.kind);
  if (artifact.mime) parts.push(artifact.mime);
  if (artifact.title) parts.push(`"${artifact.title}"`);
  return parts.join(" · ");
};

export default function RunDiffPanel(props: RunDiffPanelProps) {
  const base = String(props.baseUrl || "").trim().replace(/\/+$/, "");
  const canQuery = base.length > 0;
  const baselineKey = React.useMemo(() => `agentui.runDiffBaseline::${base}`, [base]);
  const [baselineRunId, setBaselineRunId] = useLocalStorageState<string>(baselineKey, "");

  const [runIdA, setRunIdA] = React.useState<string>("");
  const [runIdB, setRunIdB] = React.useState<string>("");
  const [loadBusyA, setLoadBusyA] = React.useState<boolean>(false);
  const [loadBusyB, setLoadBusyB] = React.useState<boolean>(false);
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
  const [evidenceBusyA, setEvidenceBusyA] = React.useState<boolean>(false);
  const [evidenceBusyB, setEvidenceBusyB] = React.useState<boolean>(false);
  const [diffOnly, setDiffOnly] = React.useState<boolean>(true);

  const loadReplay = async (
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
      const resp = await apiRunReplay(base, id, props.auth);
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "run replay failed");
      }
      setData(resp);
    } catch (err) {
      setErr(String(err));
    } finally {
      setBusy(false);
    }
  };

  const diff = React.useMemo(() => {
    if (!replayA?.bundle || !replayB?.bundle) return null;
    const requestDiffs = diffJson(replayA.bundle.request, replayB.bundle.request, MAX_DIFFS);
    const responseDiffs = diffJson(replayA.bundle.response, replayB.bundle.response, MAX_DIFFS);
    const toolDiffs = diffJson(replayA.bundle.tool_records, replayB.bundle.tool_records, MAX_DIFFS);
    const usageA = extractUsage(replayA.bundle.response);
    const usageB = extractUsage(replayB.bundle.response);
    return {
      requestDiffs,
      responseDiffs,
      toolDiffs,
      usageA,
      usageB,
    };
  }, [replayA, replayB]);

  const dbDiff = React.useMemo(() => {
    if (!dbA?.run || !dbB?.run) return null;
    const eventsA = (dbA.events || []).map(normalizeEvent);
    const eventsB = (dbB.events || []).map(normalizeEvent);
    const eventDiffs: JsonDiffEntry[] = [];
    if (eventsA.length !== eventsB.length) {
      eventDiffs.push({
        path: "events.length",
        a: eventsA.length,
        b: eventsB.length,
      });
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
    const typeDiffs: Array<{ type: string; a: number; b: number }> = [];
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
        const key = next > 1 ? `${baseKey}#${next}` : baseKey;
        map.set(key, item);
      });
      return map;
    };
    const mapA = mapWithKeys(artifactsA);
    const mapB = mapWithKeys(artifactsB);
    const keys = Array.from(new Set([...mapA.keys(), ...mapB.keys()])).sort();
    const added: Array<{ key: string; item: ReturnType<typeof normalizeArtifact> }> = [];
    const removed: Array<{ key: string; item: ReturnType<typeof normalizeArtifact> }> = [];
    const changed: Array<{ key: string; a: ReturnType<typeof normalizeArtifact>; b: ReturnType<typeof normalizeArtifact>; diffs: JsonDiffEntry[] }> = [];
    for (const key of keys) {
      const a = mapA.get(key);
      const b = mapB.get(key);
      if (a && !b) {
        removed.push({ key, item: a });
      } else if (!a && b) {
        added.push({ key, item: b });
      } else if (a && b) {
        if (artifactFingerprint(a) !== artifactFingerprint(b)) {
          const diffs = diffJsonWithPath(a.artifact_json, b.artifact_json, `artifacts.${key}`, MAX_DIFFS);
          changed.push({ key, a, b, diffs });
        }
      }
    }
    const usageDbA = extractUsageFromRunRow(dbA.run);
    const usageDbB = extractUsageFromRunRow(dbB.run);
    return {
      eventsA,
      eventsB,
      eventDiffs,
      typeDiffs,
      added,
      removed,
      changed,
      usageDbA,
      usageDbB,
    };
  }, [dbA, dbB]);

  const sameReplayHash =
    replayA?.replay_sha256 && replayB?.replay_sha256 ? replayA.replay_sha256 === replayB.replay_sha256 : false;

  const loadEvidence = async (
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
          apiGetDbRun(base, runIdNum, props.auth, {
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
        apiRunAttestation(base, id, props.auth)
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
  };

  return (
    <details
      className="mb-4 rounded-lg border border-white/10 bg-white/5 px-3 py-2"
      open={!!props.open}
      onToggle={(ev) => props.onToggle((ev.currentTarget as HTMLDetailsElement).open)}
    >
      <summary className="cursor-pointer select-none text-xs text-white/80">
        <div className="flex flex-wrap items-center justify-between gap-2">
          <div className="font-semibold text-white/80">Run diff</div>
          <div className="text-[11px] text-white/50">Compare run replay bundles (request/response/tool records)</div>
        </div>
      </summary>
      <div className="mt-3 grid gap-3">
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Run A</FieldLabel>
          <input
            className="min-w-[140px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={runIdA}
            onChange={(e) => setRunIdA(e.target.value)}
            placeholder="run_id A"
          />
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={loadBusyA || !canQuery}
            onClick={() => void loadReplay(runIdA, setLoadBusyA, setErrorA, setReplayA)}
          >
            {loadBusyA ? "Loading…" : "Load A"}
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={evidenceBusyA || !canQuery}
            onClick={() => void loadEvidence(runIdA, setEvidenceBusyA, setDbA, setDbErrA, setAttA, setAttErrA)}
          >
            {evidenceBusyA ? "Loading…" : "Load evidence"}
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!String(runIdA || "").trim()}
            onClick={() => setBaselineRunId(String(runIdA || "").trim())}
            title="Save A as baseline"
          >
            Save baseline
          </button>
        </div>

        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Run B</FieldLabel>
          <input
            className="min-w-[140px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={runIdB}
            onChange={(e) => setRunIdB(e.target.value)}
            placeholder="run_id B"
          />
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={loadBusyB || !canQuery}
            onClick={() => void loadReplay(runIdB, setLoadBusyB, setErrorB, setReplayB)}
          >
            {loadBusyB ? "Loading…" : "Load B"}
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={evidenceBusyB || !canQuery}
            onClick={() => void loadEvidence(runIdB, setEvidenceBusyB, setDbB, setDbErrB, setAttB, setAttErrB)}
          >
            {evidenceBusyB ? "Loading…" : "Load evidence"}
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!baselineRunId}
            onClick={() => setRunIdB(baselineRunId)}
            title="Load baseline into B"
          >
            Use baseline
          </button>
          {baselineRunId ? <div className="text-[10px] text-white/40">baseline: {baselineRunId}</div> : null}
        </div>

        {errorA ? (
          <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
            Run A: {errorA}
          </div>
        ) : null}
        {errorB ? (
          <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
            Run B: {errorB}
          </div>
        ) : null}
        {dbErrA ? (
          <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-[11px] text-amber-100">
            Run A DB: {dbErrA}
          </div>
        ) : null}
        {dbErrB ? (
          <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-[11px] text-amber-100">
            Run B DB: {dbErrB}
          </div>
        ) : null}
        {attErrA ? (
          <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-[11px] text-amber-100">
            Run A attestation: {attErrA}
          </div>
        ) : null}
        {attErrB ? (
          <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-[11px] text-amber-100">
            Run B attestation: {attErrB}
          </div>
        ) : null}

        {(replayA || replayB) && (
          <div className="grid gap-2 rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/70">
            {replayA ? (
              <div>
                <div>
                  A · run {replayA.run_id ?? "?"} · schema {replayA.bundle?.schema ?? "?"}
                  {replayA.replay_sha256 ? ` · ${replayA.replay_sha256}` : ""}
                </div>
                {replayA.replay_error ? <div className="text-rose-200">replay_error: {replayA.replay_error}</div> : null}
                {formatUsage(diff?.usageA) ? <div className="text-white/50">usage: {formatUsage(diff?.usageA)}</div> : null}
              </div>
            ) : null}
            {replayB ? (
              <div>
                <div>
                  B · run {replayB.run_id ?? "?"} · schema {replayB.bundle?.schema ?? "?"}
                  {replayB.replay_sha256 ? ` · ${replayB.replay_sha256}` : ""}
                </div>
                {replayB.replay_error ? <div className="text-rose-200">replay_error: {replayB.replay_error}</div> : null}
                {formatUsage(diff?.usageB) ? <div className="text-white/50">usage: {formatUsage(diff?.usageB)}</div> : null}
              </div>
            ) : null}
            {replayA && replayB ? (
              <div className={sameReplayHash ? "text-emerald-200" : "text-amber-200"}>
                replay hash {sameReplayHash ? "matches" : "differs"}
              </div>
            ) : null}
            {diff && formatUsageDelta(diff.usageA, diff.usageB) ? (
              <div className="text-white/50">usage delta: {formatUsageDelta(diff.usageA, diff.usageB)}</div>
            ) : null}
          </div>
        )}
        {(dbA || dbB || attA || attB) && (
          <div className="grid gap-2 rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/70">
            {dbA?.run ? (
              <div>
                <div>A · db run {dbA.run?.run_id ?? "?"}</div>
                {formatUsage(extractUsageFromRunRow(dbA.run)) ? (
                  <div className="text-white/50">db usage: {formatUsage(extractUsageFromRunRow(dbA.run))}</div>
                ) : null}
                {dbA.run?.replay_sha256 ? (
                  <div className="text-white/50">replay_sha256: {dbA.run?.replay_sha256}</div>
                ) : null}
              </div>
            ) : null}
            {dbB?.run ? (
              <div>
                <div>B · db run {dbB.run?.run_id ?? "?"}</div>
                {formatUsage(extractUsageFromRunRow(dbB.run)) ? (
                  <div className="text-white/50">db usage: {formatUsage(extractUsageFromRunRow(dbB.run))}</div>
                ) : null}
                {dbB.run?.replay_sha256 ? (
                  <div className="text-white/50">replay_sha256: {dbB.run?.replay_sha256}</div>
                ) : null}
              </div>
            ) : null}
            {attA?.attestation ? (
              <div className="text-white/50">
                A · attestation {attA.attestation?.schema ?? "?"} · {attA.attestation?.replay_sha256 ?? "?"}
              </div>
            ) : null}
            {attB?.attestation ? (
              <div className="text-white/50">
                B · attestation {attB.attestation?.schema ?? "?"} · {attB.attestation?.replay_sha256 ?? "?"}
              </div>
            ) : null}
            {dbDiff && formatUsageDelta(dbDiff.usageDbA, dbDiff.usageDbB) ? (
              <div className="text-white/50">db usage delta: {formatUsageDelta(dbDiff.usageDbA, dbDiff.usageDbB)}</div>
            ) : null}
          </div>
        )}

        {diff ? (
          <div className="grid gap-2 rounded-md border border-white/10 bg-black/30 p-2">
            <div className="flex items-center justify-between gap-2 text-[11px] text-white/70">
              <div>
                diffs (cap {MAX_DIFFS}) · request {diff.requestDiffs.length} · response {diff.responseDiffs.length} ·
                tool records {diff.toolDiffs.length}
              </div>
              <label className="flex items-center gap-2 text-[10px] text-white/60">
                <input
                  type="checkbox"
                  className="rounded border-white/20 bg-black/40"
                  checked={diffOnly}
                  onChange={(e) => setDiffOnly(e.target.checked)}
                />
                Diff-only view
              </label>
            </div>
            {diffOnly ? (
              <>
                {diff.requestDiffs.length > 0 ? (
                  <div className="rounded-md border border-white/5 bg-black/40 px-2 py-1 text-[10px] text-white/70">
                    request diffs:
                    {diff.requestDiffs.map((row, idx) => (
                      <div key={`req-diff-${row.path}-${idx}`}>
                        {row.path || "(root)"} · A: {formatDiffValue(row.a)} · B: {formatDiffValue(row.b)}
                      </div>
                    ))}
                  </div>
                ) : (
                  <div className="text-[10px] text-white/40">No request diffs.</div>
                )}
                {diff.responseDiffs.length > 0 ? (
                  <div className="rounded-md border border-white/5 bg-black/40 px-2 py-1 text-[10px] text-white/70">
                    response diffs:
                    {diff.responseDiffs.map((row, idx) => (
                      <div key={`resp-diff-${row.path}-${idx}`}>
                        {row.path || "(root)"} · A: {formatDiffValue(row.a)} · B: {formatDiffValue(row.b)}
                      </div>
                    ))}
                  </div>
                ) : (
                  <div className="text-[10px] text-white/40">No response diffs.</div>
                )}
                {diff.toolDiffs.length > 0 ? (
                  <div className="rounded-md border border-white/5 bg-black/40 px-2 py-1 text-[10px] text-white/70">
                    tool record diffs:
                    {diff.toolDiffs.map((row, idx) => (
                      <div key={`tool-diff-${row.path}-${idx}`}>
                        {row.path || "(root)"} · A: {formatDiffValue(row.a)} · B: {formatDiffValue(row.b)}
                      </div>
                    ))}
                  </div>
                ) : (
                  <div className="text-[10px] text-white/40">No tool record diffs.</div>
                )}
              </>
            ) : (
              <div className="grid gap-2 text-[10px] text-white/60">
                <div>
                  request A: <pre className="whitespace-pre-wrap">{stringifyJson(replayA?.bundle?.request)}</pre>
                </div>
                <div>
                  request B: <pre className="whitespace-pre-wrap">{stringifyJson(replayB?.bundle?.request)}</pre>
                </div>
                <div>
                  response A: <pre className="whitespace-pre-wrap">{stringifyJson(replayA?.bundle?.response)}</pre>
                </div>
                <div>
                  response B: <pre className="whitespace-pre-wrap">{stringifyJson(replayB?.bundle?.response)}</pre>
                </div>
                <div>
                  tool records A: <pre className="whitespace-pre-wrap">{stringifyJson(replayA?.bundle?.tool_records)}</pre>
                </div>
                <div>
                  tool records B: <pre className="whitespace-pre-wrap">{stringifyJson(replayB?.bundle?.tool_records)}</pre>
                </div>
              </div>
            )}
          </div>
        ) : (
          <div className="text-[11px] text-white/50">Load two run replays to compute a diff.</div>
        )}

        {dbDiff ? (
          <div className="grid gap-2 rounded-md border border-white/10 bg-black/30 p-2">
            <div className="text-[11px] text-white/70">
              db diffs · events {dbDiff.eventsA.length}/{dbDiff.eventsB.length} · artifacts +{dbDiff.added.length} -
              {dbDiff.removed.length} chg {dbDiff.changed.length}
            </div>
            {dbDiff.typeDiffs.length > 0 ? (
              <div className="rounded-md border border-white/5 bg-black/40 px-2 py-1 text-[10px] text-white/70">
                event type deltas:
                {dbDiff.typeDiffs.map((row) => (
                  <div key={`event-type-${row.type}`}>
                    {row.type}: A {row.a} · B {row.b}
                  </div>
                ))}
              </div>
            ) : (
              <div className="text-[10px] text-white/40">No event type deltas.</div>
            )}
            {dbDiff.eventDiffs.length > 0 ? (
              <div className="rounded-md border border-white/5 bg-black/40 px-2 py-1 text-[10px] text-white/70">
                event diffs (cap {MAX_DIFFS}):
                {dbDiff.eventDiffs.map((row, idx) => (
                  <div key={`event-diff-${row.path}-${idx}`}>
                    {row.path || "(root)"} · A: {formatDiffValue(row.a)} · B: {formatDiffValue(row.b)}
                  </div>
                ))}
              </div>
            ) : (
              <div className="text-[10px] text-white/40">No event diffs.</div>
            )}
            {dbDiff.added.length > 0 ? (
              <div className="rounded-md border border-white/5 bg-black/40 px-2 py-1 text-[10px] text-emerald-200">
                artifacts added:
                {dbDiff.added.map((row) => (
                  <div key={`artifact-add-${row.key}`}>{formatArtifactSummary(row.item)}</div>
                ))}
              </div>
            ) : null}
            {dbDiff.removed.length > 0 ? (
              <div className="rounded-md border border-white/5 bg-black/40 px-2 py-1 text-[10px] text-rose-200">
                artifacts removed:
                {dbDiff.removed.map((row) => (
                  <div key={`artifact-remove-${row.key}`}>{formatArtifactSummary(row.item)}</div>
                ))}
              </div>
            ) : null}
            {dbDiff.changed.length > 0 ? (
              <div className="rounded-md border border-white/5 bg-black/40 px-2 py-1 text-[10px] text-amber-200">
                artifacts changed:
                {dbDiff.changed.map((row) => (
                  <div key={`artifact-change-${row.key}`}>
                    {formatArtifactSummary(row.a)}
                    {row.diffs.length > 0 ? (
                      <div className="text-[9px] text-amber-100">
                        {row.diffs.slice(0, 6).map((diffRow, idx) => (
                          <div key={`artifact-change-${row.key}-${idx}`}>
                            {diffRow.path} · A: {formatDiffValue(diffRow.a)} · B: {formatDiffValue(diffRow.b)}
                          </div>
                        ))}
                        {row.diffs.length > 6 ? <div>...{row.diffs.length - 6} more</div> : null}
                      </div>
                    ) : null}
                  </div>
                ))}
              </div>
            ) : null}
            {dbDiff.added.length === 0 && dbDiff.removed.length === 0 && dbDiff.changed.length === 0 ? (
              <div className="text-[10px] text-white/40">No artifact diffs.</div>
            ) : null}
          </div>
        ) : (
          <div className="text-[11px] text-white/50">Load DB evidence to diff events/artifacts.</div>
        )}

        {!diffOnly && (attA || attB || dbA || dbB) ? (
          <div className="grid gap-2 rounded-md border border-white/10 bg-black/30 p-2 text-[10px] text-white/60">
            {dbA?.run ? (
              <div>
                db run A: <pre className="whitespace-pre-wrap">{stringifyJson(dbA.run)}</pre>
              </div>
            ) : null}
            {dbB?.run ? (
              <div>
                db run B: <pre className="whitespace-pre-wrap">{stringifyJson(dbB.run)}</pre>
              </div>
            ) : null}
            {attA?.attestation ? (
              <div>
                attestation A: <pre className="whitespace-pre-wrap">{stringifyJson(attA.attestation)}</pre>
              </div>
            ) : null}
            {attB?.attestation ? (
              <div>
                attestation B: <pre className="whitespace-pre-wrap">{stringifyJson(attB.attestation)}</pre>
              </div>
            ) : null}
          </div>
        ) : null}
      </div>
    </details>
  );
}
