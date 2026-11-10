import { safeObject } from "../../jsonUtils";
import type { RunDiffPanelState } from "./useRunDiffPanelState";
import { extractUsageFromRunRow, formatUsage, formatUsageDelta } from "./runDiffUtils";

type Props = Pick<RunDiffPanelState, "sideA" | "sideB" | "replayDiff" | "dbDiff" | "sameReplayHash">;
export default function RunDiffMetadata(props: Props) {
  const dbRunA = safeObject(props.sideA.db?.run);
  const dbRunB = safeObject(props.sideB.db?.run);
  return <div data-testid="run-diff-metadata">
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
  </div>;
}
