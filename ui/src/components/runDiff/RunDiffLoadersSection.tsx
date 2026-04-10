import FieldLabel from "../FieldLabel";
import { safeObject } from "../../jsonUtils";
import type { RunDiffPanelState } from "./useRunDiffPanelState";
import { extractUsageFromRunRow, formatUsage, formatUsageDelta } from "./runDiffUtils";

type RunDiffLoadersSectionProps = Pick<
  RunDiffPanelState,
  "canQuery" | "baselineRunId" | "setBaselineFromA" | "useBaselineForB" | "sideA" | "sideB" | "replayDiff" | "dbDiff" | "sameReplayHash"
>;

export default function RunDiffLoadersSection(props: RunDiffLoadersSectionProps) {
  const dbRunA = safeObject(props.sideA.db?.run);
  const dbRunB = safeObject(props.sideB.db?.run);

  return (
    <section className="grid gap-3" data-testid="run-diff-loaders-section">
      <div className="flex flex-wrap items-center gap-2">
        <FieldLabel>Run A</FieldLabel>
        <input
          data-testid="run-diff-run-a-input"
          className="min-w-[140px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
          value={props.sideA.runId}
          onChange={(e) => props.sideA.setRunId(e.target.value)}
          placeholder="run_id A"
        />
        <button
          className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
          type="button"
          disabled={props.sideA.loadBusy || !props.canQuery}
          onClick={() => void props.sideA.loadReplay()}
        >
          {props.sideA.loadBusy ? "Loading…" : "Load A"}
        </button>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
          type="button"
          disabled={props.sideA.evidenceBusy || !props.canQuery}
          onClick={() => void props.sideA.loadEvidence()}
        >
          {props.sideA.evidenceBusy ? "Loading…" : "Load evidence"}
        </button>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
          type="button"
          disabled={!String(props.sideA.runId || "").trim()}
          onClick={props.setBaselineFromA}
          title="Save A as baseline"
        >
          Save baseline
        </button>
      </div>

      <div className="flex flex-wrap items-center gap-2">
        <FieldLabel>Run B</FieldLabel>
        <input
          data-testid="run-diff-run-b-input"
          className="min-w-[140px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
          value={props.sideB.runId}
          onChange={(e) => props.sideB.setRunId(e.target.value)}
          placeholder="run_id B"
        />
        <button
          className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
          type="button"
          disabled={props.sideB.loadBusy || !props.canQuery}
          onClick={() => void props.sideB.loadReplay()}
        >
          {props.sideB.loadBusy ? "Loading…" : "Load B"}
        </button>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
          type="button"
          disabled={props.sideB.evidenceBusy || !props.canQuery}
          onClick={() => void props.sideB.loadEvidence()}
        >
          {props.sideB.evidenceBusy ? "Loading…" : "Load evidence"}
        </button>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
          type="button"
          disabled={!props.baselineRunId}
          onClick={props.useBaselineForB}
          title="Load baseline into B"
        >
          Use baseline
        </button>
        {props.baselineRunId ? <div className="text-[10px] text-white/40">baseline: {props.baselineRunId}</div> : null}
      </div>

      {props.sideA.error ? (
        <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
          Run A: {props.sideA.error}
        </div>
      ) : null}
      {props.sideB.error ? (
        <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
          Run B: {props.sideB.error}
        </div>
      ) : null}
      {props.sideA.dbError ? (
        <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-[11px] text-amber-100">
          Run A DB: {props.sideA.dbError}
        </div>
      ) : null}
      {props.sideB.dbError ? (
        <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-[11px] text-amber-100">
          Run B DB: {props.sideB.dbError}
        </div>
      ) : null}
      {props.sideA.attestationError ? (
        <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-[11px] text-amber-100">
          Run A attestation: {props.sideA.attestationError}
        </div>
      ) : null}
      {props.sideB.attestationError ? (
        <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-[11px] text-amber-100">
          Run B attestation: {props.sideB.attestationError}
        </div>
      ) : null}

      {(props.sideA.replay || props.sideB.replay) && (
        <div className="grid gap-2 rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/70">
          {props.sideA.replay ? (
            <div>
              <div>
                A · run {props.sideA.replay.run_id ?? "?"} · schema {props.sideA.replay.bundle?.schema ?? "?"}
                {props.sideA.replay.replay_sha256 ? ` · ${props.sideA.replay.replay_sha256}` : ""}
              </div>
              {props.sideA.replay.replay_error ? <div className="text-rose-200">replay_error: {props.sideA.replay.replay_error}</div> : null}
              {formatUsage(props.replayDiff?.usageA) ? <div className="text-white/50">usage: {formatUsage(props.replayDiff?.usageA)}</div> : null}
            </div>
          ) : null}
          {props.sideB.replay ? (
            <div>
              <div>
                B · run {props.sideB.replay.run_id ?? "?"} · schema {props.sideB.replay.bundle?.schema ?? "?"}
                {props.sideB.replay.replay_sha256 ? ` · ${props.sideB.replay.replay_sha256}` : ""}
              </div>
              {props.sideB.replay.replay_error ? <div className="text-rose-200">replay_error: {props.sideB.replay.replay_error}</div> : null}
              {formatUsage(props.replayDiff?.usageB) ? <div className="text-white/50">usage: {formatUsage(props.replayDiff?.usageB)}</div> : null}
            </div>
          ) : null}
          {props.sideA.replay && props.sideB.replay ? (
            <div className={props.sameReplayHash ? "text-emerald-200" : "text-amber-200"}>
              replay hash {props.sameReplayHash ? "matches" : "differs"}
            </div>
          ) : null}
          {props.replayDiff && formatUsageDelta(props.replayDiff.usageA, props.replayDiff.usageB) ? (
            <div className="text-white/50">usage delta: {formatUsageDelta(props.replayDiff.usageA, props.replayDiff.usageB)}</div>
          ) : null}
        </div>
      )}

      {(props.sideA.db || props.sideB.db || props.sideA.attestation || props.sideB.attestation) && (
        <div className="grid gap-2 rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/70">
          {Object.keys(dbRunA).length > 0 ? (
            <div>
              <div>A · db run {String(dbRunA.run_id ?? "?")}</div>
              {formatUsage(extractUsageFromRunRow(dbRunA)) ? (
                <div className="text-white/50">db usage: {formatUsage(extractUsageFromRunRow(dbRunA))}</div>
              ) : null}
              {dbRunA.replay_sha256 ? <div className="text-white/50">replay_sha256: {String(dbRunA.replay_sha256)}</div> : null}
            </div>
          ) : null}
          {Object.keys(dbRunB).length > 0 ? (
            <div>
              <div>B · db run {String(dbRunB.run_id ?? "?")}</div>
              {formatUsage(extractUsageFromRunRow(dbRunB)) ? (
                <div className="text-white/50">db usage: {formatUsage(extractUsageFromRunRow(dbRunB))}</div>
              ) : null}
              {dbRunB.replay_sha256 ? <div className="text-white/50">replay_sha256: {String(dbRunB.replay_sha256)}</div> : null}
            </div>
          ) : null}
          {props.sideA.attestation?.attestation ? (
            <div className="text-white/50">
              A · attestation {props.sideA.attestation.attestation?.schema ?? "?"} · {props.sideA.attestation.attestation?.replay_sha256 ?? "?"}
            </div>
          ) : null}
          {props.sideB.attestation?.attestation ? (
            <div className="text-white/50">
              B · attestation {props.sideB.attestation.attestation?.schema ?? "?"} · {props.sideB.attestation.attestation?.replay_sha256 ?? "?"}
            </div>
          ) : null}
          {props.dbDiff && formatUsageDelta(props.dbDiff.usageDbA, props.dbDiff.usageDbB) ? (
            <div className="text-white/50">db usage delta: {formatUsageDelta(props.dbDiff.usageDbA, props.dbDiff.usageDbB)}</div>
          ) : null}
        </div>
      )}
    </section>
  );
}
