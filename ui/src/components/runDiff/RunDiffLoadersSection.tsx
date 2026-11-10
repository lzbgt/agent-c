import { ToolError } from "../ToolOutput";
import type { RunDiffPanelState } from "./useRunDiffPanelState";

type Props = Pick<RunDiffPanelState, "canQuery" | "baselineRunId" | "setBaselineFromA" | "useBaselineForB" | "sideA" | "sideB">;

export default function RunDiffLoadersSection(props: Props) {
  return <section className="grid gap-3" data-testid="run-diff-loaders-section">
    {([ ["A", props.sideA], ["B", props.sideB] ] as const).map(([name, side]) => <div key={name}>
      <form className="tool-form-row" onSubmit={(event) => { event.preventDefault(); if (props.canQuery && !side.loadBusy) void side.loadReplay(); }}>
        <label className="tool-field tool-grow">Run {name}
          <input data-testid={`run-diff-run-${name.toLowerCase()}-input`} value={side.runId} onChange={(e) => side.setRunId(e.target.value)} placeholder={`Run ID ${name}`} />
        </label>
        <button className="workspace-button" type="submit" disabled={side.loadBusy || !props.canQuery || !side.runId.trim()}>{side.loadBusy ? "Loading…" : `Load ${name}`}</button>
      </form>
      <ToolError error={side.error ? `Run ${name}: ${side.error}` : null} />
      <ToolError error={side.dbError ? `Run ${name} evidence: ${side.dbError}` : null} />
      <ToolError error={side.attestationError ? `Run ${name} attestation: ${side.attestationError}` : null} />
    </div>)}
    <details className="tool-details" data-testid="run-diff-evidence-options">
      <summary>Evidence & baseline</summary>
      <div className="tool-form-row">
        {([ ["A", props.sideA], ["B", props.sideB] ] as const).map(([name, side]) => <button key={name} className="workspace-button" type="button" disabled={side.evidenceBusy || !props.canQuery || !side.runId.trim()} onClick={() => void side.loadEvidence()}>{side.evidenceBusy ? "Loading…" : `Load evidence ${name}`}</button>)}
        <button className="workspace-button" type="button" disabled={!props.sideA.runId.trim()} onClick={props.setBaselineFromA}>Save A as baseline</button>
        <button className="workspace-button" type="button" disabled={!props.baselineRunId} onClick={props.useBaselineForB}>Use baseline for B</button>
      </div>
      {props.baselineRunId ? <p className="tool-muted">Saved baseline: {props.baselineRunId}</p> : null}
    </details>
  </section>;
}
