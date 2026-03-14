import type { RunDiffPanelState } from "./useRunDiffPanelState";
import { MAX_DIFFS, formatArtifactSummary, formatDiffValue, stringifyJson } from "./runDiffUtils";

type RunDiffEvidenceSectionProps = Pick<RunDiffPanelState, "dbDiff" | "diffOnly" | "sideA" | "sideB">;

export default function RunDiffEvidenceSection(props: RunDiffEvidenceSectionProps) {
  if (!props.dbDiff) {
    return <div className="text-[11px] text-white/50">Load DB evidence to diff events/artifacts.</div>;
  }

  return (
    <>
      <section className="grid gap-2 rounded-md border border-white/10 bg-black/30 p-2" data-testid="run-diff-evidence-section">
        <div className="text-[11px] text-white/70">
          db diffs · events {props.dbDiff.eventsA.length}/{props.dbDiff.eventsB.length} · artifacts +{props.dbDiff.added.length} -
          {props.dbDiff.removed.length} chg {props.dbDiff.changed.length}
        </div>
        {props.dbDiff.typeDiffs.length > 0 ? (
          <div className="rounded-md border border-white/5 bg-black/40 px-2 py-1 text-[10px] text-white/70">
            event type deltas:
            {props.dbDiff.typeDiffs.map((row) => (
              <div key={`event-type-${row.type}`}>
                {row.type}: A {row.a} · B {row.b}
              </div>
            ))}
          </div>
        ) : (
          <div className="text-[10px] text-white/40">No event type deltas.</div>
        )}
        {props.dbDiff.eventDiffs.length > 0 ? (
          <div className="rounded-md border border-white/5 bg-black/40 px-2 py-1 text-[10px] text-white/70">
            event diffs (cap {MAX_DIFFS}):
            {props.dbDiff.eventDiffs.map((row, idx) => (
              <div key={`event-diff-${row.path}-${idx}`}>
                {row.path || "(root)"} · A: {formatDiffValue(row.a)} · B: {formatDiffValue(row.b)}
              </div>
            ))}
          </div>
        ) : (
          <div className="text-[10px] text-white/40">No event diffs.</div>
        )}
        {props.dbDiff.added.length > 0 ? (
          <div className="rounded-md border border-white/5 bg-black/40 px-2 py-1 text-[10px] text-emerald-200">
            artifacts added:
            {props.dbDiff.added.map((row) => (
              <div key={`artifact-add-${row.key}`}>{formatArtifactSummary(row.item)}</div>
            ))}
          </div>
        ) : null}
        {props.dbDiff.removed.length > 0 ? (
          <div className="rounded-md border border-white/5 bg-black/40 px-2 py-1 text-[10px] text-rose-200">
            artifacts removed:
            {props.dbDiff.removed.map((row) => (
              <div key={`artifact-remove-${row.key}`}>{formatArtifactSummary(row.item)}</div>
            ))}
          </div>
        ) : null}
        {props.dbDiff.changed.length > 0 ? (
          <div className="rounded-md border border-white/5 bg-black/40 px-2 py-1 text-[10px] text-amber-200">
            artifacts changed:
            {props.dbDiff.changed.map((row) => (
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
        {props.dbDiff.added.length === 0 && props.dbDiff.removed.length === 0 && props.dbDiff.changed.length === 0 ? (
          <div className="text-[10px] text-white/40">No artifact diffs.</div>
        ) : null}
      </section>

      {!props.diffOnly && (props.sideA.attestation || props.sideB.attestation || props.sideA.db || props.sideB.db) ? (
        <section className="grid gap-2 rounded-md border border-white/10 bg-black/30 p-2 text-[10px] text-white/60">
          {props.sideA.db?.run ? (
            <div>
              db run A: <pre className="whitespace-pre-wrap">{stringifyJson(props.sideA.db.run)}</pre>
            </div>
          ) : null}
          {props.sideB.db?.run ? (
            <div>
              db run B: <pre className="whitespace-pre-wrap">{stringifyJson(props.sideB.db.run)}</pre>
            </div>
          ) : null}
          {props.sideA.attestation?.attestation ? (
            <div>
              attestation A: <pre className="whitespace-pre-wrap">{stringifyJson(props.sideA.attestation.attestation)}</pre>
            </div>
          ) : null}
          {props.sideB.attestation?.attestation ? (
            <div>
              attestation B: <pre className="whitespace-pre-wrap">{stringifyJson(props.sideB.attestation.attestation)}</pre>
            </div>
          ) : null}
        </section>
      ) : null}
    </>
  );
}
