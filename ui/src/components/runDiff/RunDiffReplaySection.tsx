import type { RunDiffPanelState } from "./useRunDiffPanelState";
import { MAX_DIFFS, formatDiffValue, stringifyJson } from "./runDiffUtils";

type RunDiffReplaySectionProps = Pick<RunDiffPanelState, "diffOnly" | "setDiffOnly" | "replayDiff" | "sideA" | "sideB">;

export default function RunDiffReplaySection(props: RunDiffReplaySectionProps) {
  if (!props.replayDiff) {
    return <div className="text-[11px] text-white/50">Load two run replays to compute a diff.</div>;
  }

  return (
    <section className="grid gap-2 rounded-md border border-white/10 bg-black/30 p-2" data-testid="run-diff-replay-section">
      <div className="flex items-center justify-between gap-2 text-[11px] text-white/70">
        <div>
          diffs (cap {MAX_DIFFS}) · request {props.replayDiff.requestDiffs.length} · response {props.replayDiff.responseDiffs.length} · tool records{" "}
          {props.replayDiff.toolDiffs.length}
        </div>
        <label className="flex items-center gap-2 text-[10px] text-white/60">
          <input
            type="checkbox"
            className="rounded border-white/20 bg-black/40"
            checked={props.diffOnly}
            onChange={(e) => props.setDiffOnly(e.target.checked)}
          />
          Diff-only view
        </label>
      </div>
      {props.diffOnly ? (
        <>
          {props.replayDiff.requestDiffs.length > 0 ? (
            <div className="rounded-md border border-white/5 bg-black/40 px-2 py-1 text-[10px] text-white/70">
              request diffs:
              {props.replayDiff.requestDiffs.map((row, idx) => (
                <div key={`req-diff-${row.path}-${idx}`}>
                  {row.path || "(root)"} · A: {formatDiffValue(row.a)} · B: {formatDiffValue(row.b)}
                </div>
              ))}
            </div>
          ) : (
            <div className="text-[10px] text-white/40">No request diffs.</div>
          )}
          {props.replayDiff.responseDiffs.length > 0 ? (
            <div className="rounded-md border border-white/5 bg-black/40 px-2 py-1 text-[10px] text-white/70">
              response diffs:
              {props.replayDiff.responseDiffs.map((row, idx) => (
                <div key={`resp-diff-${row.path}-${idx}`}>
                  {row.path || "(root)"} · A: {formatDiffValue(row.a)} · B: {formatDiffValue(row.b)}
                </div>
              ))}
            </div>
          ) : (
            <div className="text-[10px] text-white/40">No response diffs.</div>
          )}
          {props.replayDiff.toolDiffs.length > 0 ? (
            <div className="rounded-md border border-white/5 bg-black/40 px-2 py-1 text-[10px] text-white/70">
              tool record diffs:
              {props.replayDiff.toolDiffs.map((row, idx) => (
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
            request A: <pre className="whitespace-pre-wrap">{stringifyJson(props.sideA.replay?.bundle?.request)}</pre>
          </div>
          <div>
            request B: <pre className="whitespace-pre-wrap">{stringifyJson(props.sideB.replay?.bundle?.request)}</pre>
          </div>
          <div>
            response A: <pre className="whitespace-pre-wrap">{stringifyJson(props.sideA.replay?.bundle?.response)}</pre>
          </div>
          <div>
            response B: <pre className="whitespace-pre-wrap">{stringifyJson(props.sideB.replay?.bundle?.response)}</pre>
          </div>
          <div>
            tool records A: <pre className="whitespace-pre-wrap">{stringifyJson(props.sideA.replay?.bundle?.tool_records)}</pre>
          </div>
          <div>
            tool records B: <pre className="whitespace-pre-wrap">{stringifyJson(props.sideB.replay?.bundle?.tool_records)}</pre>
          </div>
        </div>
      )}
    </section>
  );
}
