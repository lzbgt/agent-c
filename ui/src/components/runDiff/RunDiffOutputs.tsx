import Markdown from "../Markdown";
import { ToolError } from "../ToolOutput";
import { safeObject } from "../../jsonUtils";
import type { RunDiffPanelState } from "./useRunDiffPanelState";

export default function RunDiffOutputs({ sideA, sideB, dbDiff }: Pick<RunDiffPanelState, "sideA" | "sideB" | "dbDiff">) {
  const text = (side: typeof sideA) => {
    const response = safeObject(side.replay?.bundle?.response);
    return typeof response.assistant_text === "string" ? response.assistant_text : "";
  };
  const a = text(sideA), b = text(sideB);
  return <section aria-label="Run outputs" data-testid="run-diff-outputs">
    {sideA.replay && sideB.replay && a && b ? <p className="tool-muted">{a === b ? "Assistant outputs match." : "Assistant outputs differ."}</p> : null}
    <div className="tool-output-columns">
      {([ ["A", sideA, a], ["B", sideB, b] ] as const).map(([name, side, output]) => side.replay ? <article className="tool-card" key={name}>
        <h3>Run {name} · {side.replay.run_id ?? "loaded run"}</h3>
        {typeof safeObject(side.replay.bundle?.request).prompt === "string" ? <div className="tool-preview"><p className="tool-muted">You</p><Markdown text={String(safeObject(side.replay.bundle?.request).prompt)} /></div> : null}
        <div className="tool-preview"><p className="tool-muted">Agent</p>{output ? <Markdown text={output} /> : <p className="tool-muted">No assistant output recorded.</p>}</div>
        <ToolError error={typeof safeObject(side.replay.bundle?.response).error === "string" ? String(safeObject(side.replay.bundle?.response).error) : side.replay.replay_error || null} />
      </article> : null)}
    </div>
    {!sideA.replay && !sideB.replay ? <p className="tool-empty">Load two runs to compare their messages and results.</p> : null}
    {dbDiff ? <div className="mt-4">
      <h3>Artifacts in loaded evidence</h3>
      <p className="tool-muted">Run A {String(safeObject(sideA.db?.run).run_id ?? "unknown")} · Run B {String(safeObject(sideB.db?.run).run_id ?? "unknown")}</p>
      {dbDiff.added.map((row) => <p key={`added-${row.key}`}>Added · {row.item.path || row.item.title || row.key}</p>)}
      {dbDiff.removed.map((row) => <p key={`removed-${row.key}`}>Removed · {row.item.path || row.item.title || row.key}</p>)}
      {dbDiff.changed.map((row) => <p key={`changed-${row.key}`}>Changed · {row.b.path || row.b.title || row.key}</p>)}
      {!dbDiff.added.length && !dbDiff.removed.length && !dbDiff.changed.length ? <p className="tool-muted">No artifact changes.</p> : null}
    </div> : null}
  </section>;
}
