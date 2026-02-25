import React from "react";
import { apiRunReplay, type ApiAuth, type RunReplayResp } from "../api";
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

const formatUsage = (usage?: Record<string, number> | null) => {
  if (!usage) return "";
  const parts: string[] = [];
  if (typeof usage.total_tokens === "number") parts.push(`tokens ${usage.total_tokens}`);
  if (typeof usage.prompt_tokens === "number") parts.push(`prompt ${usage.prompt_tokens}`);
  if (typeof usage.completion_tokens === "number") parts.push(`completion ${usage.completion_tokens}`);
  if (typeof usage.total_cost === "number") parts.push(`cost ${usage.total_cost.toFixed(4)}`);
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

  const sameReplayHash =
    replayA?.replay_sha256 && replayB?.replay_sha256 ? replayA.replay_sha256 === replayB.replay_sha256 : false;

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
      </div>
    </details>
  );
}
